/**
 * @file  OS_Timer.c
 * @brief Software-timer pool with timing-wheel – all O(1) operations.
 *
 * @par Architecture
 *
 * Active timers live in a hashed timing wheel indexed by
 * (Expiry & OS_TIMER_WHEEL_MASK).  Each timer also carries a Round
 * counter: the number of full wheel rotations remaining before the
 * timer fires.  OS_SysTick inspects exactly one wheel slot per tick —
 * O(1) per tick (bounded by OS_TIMER_WHEEL_SIZE = pool size).
 *
 * Operations:
 *   - OS_TimerCreate      : O(1) alloc + wheel insert.
 *   - OS_TimerDelete      : O(1) doubly-linked unlink.
 *   - OS_TimerRestart     : O(1) wheel remove + reinsert (no alloc).
 *   - OS_SysTick          : O(1) — one slot per tick, at most
 *                           OS_TIMER_WHEEL_SIZE timers per slot.
 *   - OS_TimerDeleteByState: O(m) in timers owned by that state,
 *                            bounded by OS_TIMER_MAX_PER_HSM.
 *                            Called only during transitions, never
 *                            from the tick ISR.  Intentional.
 *
 * @par Long-period timers (fix 1.1)
 * The Round counter replaces the old TickLeq signed-comparison trick.
 * Unsigned arithmetic is used throughout; no signed overflow.
 * Formula: Round = (Expiry - TickCounter - 1) >> OS_TIMER_WHEEL_BITS
 * This correctly handles periods up to 2^32 - 1 ticks.
 * For periods beyond 49 days at 1 ms/tick, increase OS_TICK_PERIOD_MS.
 *
 * @par Tick ISR design (fix 1.2)
 * OS_SysTick only inserts events into the queue (OS_InsertEventFromIsr).
 * It never dispatches.  Event dispatch runs in OS_EventDispatch() at
 * lower priority (the "OS IRQ" above the main while(1) loop).
 * This guarantees the tick ISR completes in bounded time.
 *
 * @par Per-HSM quota (fix 3.6)
 * Each HSM is limited to OS_TIMER_MAX_PER_HSM simultaneous timers,
 * preventing a single HSM from exhausting the shared pool.
 *
 * @par Generation counter (fix 2.7)
 * Active timers always have Generation >= 1.  Generation 0 is never
 * assigned to an active timer, so OS_TIMER_INVALID {0xFFFF, 0} is safe.
 * On U16 wrap-around the counter skips 0 and resumes at 1.
 */
#include "OS_Timer.h"
#include "OS_Timer_Private.h"
#include "OS_Watchdog_Private.h"
#include "OS_Event.h"
#include "OS_Error.h"
#include "OS_Hsm.h"
#include "OS_Config.h"
#include "OS_Port.h"

/* ------------------------------------------------------------------ */
/*  Compile-time invariant checks                                     */
/* ------------------------------------------------------------------ */

_Static_assert((OS_TIMER_WHEEL_SIZE & (OS_TIMER_WHEEL_SIZE - 1U)) == 0U,
               "OS_TIMER_WHEEL_SIZE must be a power of two");
_Static_assert(OS_TIMER_WHEEL_SIZE >= 2U,
               "OS_TIMER_WHEEL_SIZE must be at least 2");
_Static_assert(OS_TIMER_WHEEL_SIZE <= 32767U,
               "OS_TIMER_WHEEL_SIZE must fit in OS_I16 (max 32767)");
_Static_assert(OS_TIMER_MAX_PER_HSM <= OS_TIMER_WHEEL_SIZE,
               "OS_TIMER_MAX_PER_HSM cannot exceed OS_TIMER_WHEEL_SIZE");
_Static_assert(OS_TIMER_MAX_PER_HSM >= 1U,
               "OS_TIMER_MAX_PER_HSM must be at least 1");
_Static_assert(OS_TIMER_MAX_PER_HSM <= 255U,
               "OS_TIMER_MAX_PER_HSM must fit in OS_U8 (TimerCount)");

/* ------------------------------------------------------------------ */
/*  Timer block                                                       */
/* ------------------------------------------------------------------ */

/** @brief Internal representation of a single software timer. */
typedef struct {
    OS_U32          Expiry;       /**< Absolute tick of next expiry.  */
    OS_U32          Period;       /**< Reload value (0 = one-shot).   */
    OS_U32          Round;        /**< Full wheel rotations before firing;
                                       computed on insert/reschedule.   */
    OS_Signal       Signal;       /**< Signal posted on expiry.       */
    OS_Hsm         *Hook;         /**< Owning HSM.                    */
    OS_StateHandler OwnerState;   /**< State that created this timer. */
    OS_U16          Generation;   /**< Stale-handle detection (>= 1). */
    OS_I16          NextFree;     /**< Free-list link (-1 = end).     */
    OS_I16          NextHsm;      /**< Per-HSM list forward link.     */
    OS_I16          PrevHsm;      /**< Per-HSM list backward link.    */
    OS_I16          NextWheel;    /**< Wheel-slot list fwd link.      */
    OS_I16          PrevWheel;    /**< Wheel-slot list bwd link.      */
    bool            Active;       /**< true while timer is running.   */
} TimerBlock;

/* ------------------------------------------------------------------ */
/*  Module-private state                                              */
/* ------------------------------------------------------------------ */

static TimerBlock Pool[OS_TIMER_WHEEL_SIZE];
static OS_I16     FreeHead;
static OS_I16     Wheel[OS_TIMER_WHEEL_SIZE]; /**< Timing-wheel slots. */

/* Tick counter */
static volatile OS_U32 TickCounter;

/* ------------------------------------------------------------------ */
/*  Private helpers                                                   */
/* ------------------------------------------------------------------ */

/**
 * @brief Insert a timer into its timing-wheel slot – O(1).
 *
 * Computes the Round counter using unsigned arithmetic so that
 * arbitrarily large periods (up to 2^32 - 1 ticks) work correctly.
 * Formula: Round = (Expiry - TickCounter - 1) >> OS_TIMER_WHEEL_BITS
 *
 * @param idx  Pool slot index.
 */
static void WheelInsert(OS_U16 idx)
{
    OS_U32 delta;
    OS_U16 slot;

    /*
     * Round formula explained:
     *   The wheel slot is visited every OS_TIMER_WHEEL_SIZE ticks.
     *   We want the timer to fire on the visit where TickCounter == Expiry
     *   (modulo WHEEL_SIZE).  Round counts how many full rotations must
     *   pass BEFORE the firing visit.
     *
     *   Let delta = Expiry - TickCounter (unsigned, always >= 1 because
     *   Expiry = TickCounter + periodTicks and periodTicks >= 1, which
     *   is guaranteed by the assertion in OS_TimerCreate).
     *
     *   The slot is first visited (delta - 1) ticks later or less, so:
     *     Round = (delta - 1) / WHEEL_SIZE
     *   Using bit-shift: Round = (delta - 1U) >> OS_TIMER_WHEEL_BITS.
     *
     *   Verification with WHEEL_SIZE = 16:
     *     delta = 1  → Round = 0, fires next slot visit (1 tick away). ✓
     *     delta = 16 → Round = 0, fires at exactly the next slot visit. ✓
     *     delta = 17 → Round = 1, fires one rotation later.            ✓
     */
    delta = Pool[idx].Expiry - TickCounter - 1U;
    Pool[idx].Round = delta >> (OS_U32)OS_TIMER_WHEEL_BITS;

    slot = (OS_U16)(Pool[idx].Expiry & (OS_U32)OS_TIMER_WHEEL_MASK);

    Pool[idx].PrevWheel = -1;
    Pool[idx].NextWheel = Wheel[slot];
    if (Wheel[slot] >= 0) {
        Pool[(OS_U16)Wheel[slot]].PrevWheel = (OS_I16)idx;
    }
    Wheel[slot] = (OS_I16)idx;
}

/**
 * @brief Remove a timer from its timing-wheel slot – O(1).
 * @param idx  Pool slot index.
 */
static void WheelRemove(OS_U16 idx)
{
    OS_U16 slot = (OS_U16)(Pool[idx].Expiry & (OS_U32)OS_TIMER_WHEEL_MASK);

    if (Pool[idx].PrevWheel >= 0) {
        Pool[(OS_U16)Pool[idx].PrevWheel].NextWheel = Pool[idx].NextWheel;
    } else {
        Wheel[slot] = Pool[idx].NextWheel;
    }
    if (Pool[idx].NextWheel >= 0) {
        Pool[(OS_U16)Pool[idx].NextWheel].PrevWheel = Pool[idx].PrevWheel;
    }
    Pool[idx].NextWheel = -1;
    Pool[idx].PrevWheel = -1;
}

/**
 * @brief Return a pool slot to the free list – O(1).
 * @param idx  Slot index.
 */
static void TimerFree(OS_U16 idx)
{
    /* Unlink from timing-wheel slot – O(1). */
    WheelRemove(idx);

    /* Unlink from per-HSM doubly-linked list – O(1). */
    if (Pool[idx].PrevHsm >= 0) {
        Pool[(OS_U16)Pool[idx].PrevHsm].NextHsm = Pool[idx].NextHsm;
    } else {
        Pool[idx].Hook->TimerHead = Pool[idx].NextHsm;
    }
    if (Pool[idx].NextHsm >= 0) {
        Pool[(OS_U16)Pool[idx].NextHsm].PrevHsm = Pool[idx].PrevHsm;
    }

    /* Update per-HSM timer count. */
    Pool[idx].Hook->TimerCount--;

    /* Clear all fields to prevent stale-data access. */
    Pool[idx].Active      = false;
    Pool[idx].Hook        = (OS_Hsm *)0;
    Pool[idx].OwnerState  = (OS_StateHandler)0;
    Pool[idx].Expiry      = 0U;
    Pool[idx].Period      = 0U;
    Pool[idx].Round       = 0U;
    Pool[idx].Signal      = (OS_Signal)Q_EMPTY;
    Pool[idx].NextHsm     = -1;
    Pool[idx].PrevHsm     = -1;
    Pool[idx].NextWheel   = -1;
    Pool[idx].PrevWheel   = -1;
    Pool[idx].NextFree    = FreeHead;
    FreeHead              = (OS_I16)idx;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

void OS_TimerInit(void)
{
    OS_U16 i;

    for (i = 0U; i < (OS_U16)OS_TIMER_WHEEL_SIZE; i++) {
        Pool[i].Active      = false;
        Pool[i].Generation  = 0U;
        Pool[i].NextFree    = (OS_I16)(i + 1U);
        Pool[i].NextHsm     = -1;
        Pool[i].PrevHsm     = -1;
        Pool[i].NextWheel   = -1;
        Pool[i].PrevWheel   = -1;
        Pool[i].Hook        = (OS_Hsm *)0;
        Pool[i].OwnerState  = (OS_StateHandler)0;
        Pool[i].Expiry      = 0U;
        Pool[i].Period      = 0U;
        Pool[i].Round       = 0U;
        Pool[i].Signal      = (OS_Signal)Q_EMPTY;
    }
    Pool[OS_TIMER_WHEEL_SIZE - 1U].NextFree = -1;

    for (i = 0U; i < (OS_U16)OS_TIMER_WHEEL_SIZE; i++) {
        Wheel[i] = -1;
    }

    FreeHead    = 0;
    TickCounter = 0U;
}

/* ------------------------------------------------------------------ */
OS_TimerHandle OS_TimerCreate(OS_Signal signal,
                              OS_U32 periodTicks, bool periodic)
{
    OS_TimerHandle handle;
    OS_U16         idx;
    OS_Hsm        *me;
    OS_U16         newGen;

    Q_ASSERT(OS_HsmInDispatch());
    me = OS_HsmGetCurrent();
    Q_ASSERT(me != (OS_Hsm *)0);
    Q_ASSERT(me->Initialized);
    Q_ASSERT(periodTicks > 0U);

    Port_CriticalEnter();

    /* Per-HSM quota check – O(1). */
    Q_ASSERT(me->TimerCount < (OS_U8)OS_TIMER_MAX_PER_HSM);

    /*
     * Duplicate-signal check: walk this HSM's timer list.
     * Bounded by OS_TIMER_MAX_PER_HSM — effectively O(1).
     * Intentional: Q_ASSERT on duplicate (fail-fast).
     */
    {
        OS_I16 cur = me->TimerHead;
        while (cur >= 0) {
            Q_ASSERT(Pool[(OS_U16)cur].Signal != signal);
            cur = Pool[(OS_U16)cur].NextHsm;
        }
    }

    /* Allocate from free list – O(1). */
    Q_ASSERT(FreeHead >= 0);

    idx      = (OS_U16)FreeHead;
    FreeHead = Pool[idx].NextFree;

    /* Generation: always >= 1; skip 0 on wrap to distinguish from INVALID. */
    newGen = Pool[idx].Generation + 1U;
    if (newGen == 0U) {
        newGen = 1U;
    }
    Pool[idx].Generation = newGen;

    Pool[idx].Expiry     = TickCounter + periodTicks;
    Pool[idx].Period     = periodic ? periodTicks : 0U;
    Pool[idx].Signal     = signal;
    Pool[idx].Hook       = me;
    Pool[idx].OwnerState = me->State[OS_HsmGetDispatchDepth()];
    Pool[idx].Active     = true;

    /* Insert at head of per-HSM list – O(1). */
    Pool[idx].NextHsm = me->TimerHead;
    Pool[idx].PrevHsm = -1;
    if (me->TimerHead >= 0) {
        Pool[(OS_U16)me->TimerHead].PrevHsm = (OS_I16)idx;
    }
    me->TimerHead = (OS_I16)idx;
    me->TimerCount++;

    /* Insert into timing-wheel slot (computes Round) – O(1). */
    WheelInsert(idx);

    handle.Index      = idx;
    handle.Generation = Pool[idx].Generation;

    Port_CriticalExit();

    return handle;
}

/* ------------------------------------------------------------------ */
bool OS_TimerDelete(OS_TimerHandle handle)
{
    TimerBlock *t;
    bool        deleted = false;

    if (handle.Index >= (OS_U16)OS_TIMER_WHEEL_SIZE) {
        return false;   /* Index out of pool bounds — reject regardless of generation. */
    }

    Port_CriticalEnter();

    t = &Pool[handle.Index];

    if (t->Active && (t->Generation == handle.Generation)) {
        /* Protection: only the owning state may delete. */
        Q_ASSERT(OS_HsmInDispatch());
        Q_ASSERT(t->Hook == OS_HsmGetCurrent());
        Q_ASSERT(t->OwnerState
                 == t->Hook->State[OS_HsmGetDispatchDepth()]);

        TimerFree(handle.Index);
        deleted = true;
    }
    /* If handle is stale (timer expired or already deleted): return false
     * without asserting.  This is the correct behaviour — the signal may
     * already be in the queue, but no further action is needed. */

    Port_CriticalExit();

    return deleted;
}

/* ------------------------------------------------------------------ */
bool OS_TimerRestart(OS_TimerHandle handle, OS_U32 newPeriodTicks)
{
    TimerBlock *t;
    bool        restarted = false;

    Q_ASSERT(newPeriodTicks > 0U);

    if (handle.Index >= (OS_U16)OS_TIMER_WHEEL_SIZE) {
        return false;
    }

    Port_CriticalEnter();

    t = &Pool[handle.Index];

    if (t->Active && (t->Generation == handle.Generation)) {
        /* Protection: only the owning state may restart. */
        Q_ASSERT(OS_HsmInDispatch());
        Q_ASSERT(t->Hook == OS_HsmGetCurrent());
        Q_ASSERT(t->OwnerState
                 == t->Hook->State[OS_HsmGetDispatchDepth()]);

        /* Move to the new wheel slot without touching free/HSM lists. */
        WheelRemove(handle.Index);
        t->Expiry = TickCounter + newPeriodTicks;
        if (t->Period > 0U) {
            t->Period = newPeriodTicks;
        }
        WheelInsert(handle.Index);

        restarted = true;
    }

    Port_CriticalExit();

    return restarted;
}

/* ------------------------------------------------------------------ */
/* OS-internal: declared in OS_Timer_Private.h, not in public header. */
void OS_TimerDeleteByState(void)
{
    OS_Hsm         *hook;
    OS_StateHandler state;
    OS_I16          cur;
    OS_I16          next;

    hook  = OS_HsmGetCurrent();
    state = hook->State[OS_HsmGetDispatchDepth()];

    Port_CriticalEnter();

    cur = hook->TimerHead;
    while (cur >= 0) {
        next = Pool[(OS_U16)cur].NextHsm;
        if (Pool[(OS_U16)cur].OwnerState == state) {
            TimerFree((OS_U16)cur);
        }
        cur = next;
    }

    Port_CriticalExit();
}

/* ------------------------------------------------------------------ */
void OS_SysTick(void)
{
    OS_I16 cur;
    OS_I16 next;
    OS_U16 slot;

    Port_CriticalEnter();

    TickCounter++;

    /*
     * Inspect the single wheel slot for this tick – O(1).
     * For each timer in the slot:
     *   Round > 0  → decrement (one fewer rotation to go).
     *   Round == 0 → timer expired: insert event, reschedule/free.
     *
     * The critical section covers only event insertion and wheel
     * updates (no dispatch).  This is the highest-priority ISR.
     */
    slot = (OS_U16)(TickCounter & (OS_U32)OS_TIMER_WHEEL_MASK);
    cur  = Wheel[slot];

    while (cur >= 0) {
        next = Pool[(OS_U16)cur].NextWheel;

        if (Pool[(OS_U16)cur].Round > 0U) {
            Pool[(OS_U16)cur].Round--;
        } else {
            /* Timer expired: insert event into queue (no dispatch). */
            OS_InsertEventFromIsr(Pool[(OS_U16)cur].Signal,
                                  Pool[(OS_U16)cur].Hook);

            if (Pool[(OS_U16)cur].Period > 0U) {
                /* Periodic: reschedule — move to the new wheel slot. */
                WheelRemove((OS_U16)cur);
                Pool[(OS_U16)cur].Expiry =
                    Pool[(OS_U16)cur].Expiry + Pool[(OS_U16)cur].Period;
                WheelInsert((OS_U16)cur);
            } else {
                TimerFree((OS_U16)cur);
            }
        }

        cur = next;
    }

    OS_WatchdogTick();

    Port_CriticalExit();
}

/* ------------------------------------------------------------------ */
OS_U32 OS_GetTickCount(void)
{
    OS_U32 count;

    /* Critical section for atomic read on 8/16-bit platforms. */
    Port_CriticalEnter();
    count = TickCounter;
    Port_CriticalExit();

    return count;
}
