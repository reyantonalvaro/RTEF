/**
 * @file  OS_Timer.c
 * @brief Software-timer pool with timing-wheel – all O(1) operations.
 *
 * Active timers live in a hashed timing wheel indexed by
 * (Expiry & OS_TIMER_WHEEL_MASK).  Every operation is O(1):
 *   - OS_TimerCreate : hash + insert at slot head.
 *   - OS_TimerDelete : doubly-linked unlink.
 *   - OS_SysTick     : inspect one wheel slot per tick.
 * The sole exception is OS_TimerDeleteByState which walks the
 * per-HSM list and is O(m) in the number of timers owned by that HSM.
 */
#include "OS_Timer.h"
#include "OS_Event.h"
#include "OS_Error.h"
#include "OS_Hsm.h"
#include "OS_Config.h"
#include "OS_Port.h"

/* ------------------------------------------------------------------ */
/*  Timer block                                                       */
/* ------------------------------------------------------------------ */

/** @brief Internal representation of a single software timer. */
typedef struct {
    OS_U32          Expiry;       /**< Absolute tick of next expiry.  */
    OS_U32          Period;       /**< Reload value (0 = one-shot).   */
    OS_Signal       Signal;       /**< Signal posted on expiry.       */
    OS_Hsm         *Hook;         /**< Owning HSM.                    */
    OS_StateHandler OwnerState;   /**< State that created this timer. */
    OS_U16          Generation;   /**< Stale-handle detection.        */
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

static TimerBlock Pool[OS_MAX_TIMERS];
static OS_I16     FreeHead;
static OS_I16     Wheel[OS_TIMER_WHEEL_SIZE]; /**< Timing-wheel slots. */

/* Watchdog */
static OS_U32 WdgTimeout;
static OS_U32 WdgCounter;
static bool   WdgEnabled;

/* Tick counter */
static volatile OS_U32 TickCounter;

/* ------------------------------------------------------------------ */
/*  Private helpers                                                   */
/* ------------------------------------------------------------------ */

/**
 * @brief True when absolute tick @p a is at or before @p b (handles wrap).
 */
static bool TickLeq(OS_U32 a, OS_U32 b)
{
    return (OS_I32)(a - b) <= 0;
}

/**
 * @brief Insert a timer into its timing-wheel slot – O(1).
 * @param idx  Pool slot index.
 */
static void WheelInsert(OS_U16 idx)
{
    OS_U16 slot = (OS_U16)(Pool[idx].Expiry & (OS_U32)OS_TIMER_WHEEL_MASK);

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

    Pool[idx].Active    = false;
    Pool[idx].NextHsm   = -1;
    Pool[idx].PrevHsm   = -1;
    Pool[idx].NextWheel  = -1;
    Pool[idx].PrevWheel  = -1;
    Pool[idx].NextFree   = FreeHead;
    FreeHead             = (OS_I16)idx;
}

/**
 * @brief Advance the software watchdog by one tick.
 */
static void WatchdogTick(void)
{
    if (!WdgEnabled) {
        return;
    }

    if (WdgCounter > 0U) {
        WdgCounter--;
    }

    if (WdgCounter == 0U) {
        Q_ASSERT_ID(99U, false);   /* Software watchdog expired */
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

void OS_TimerInit(void)
{
    OS_U16 i;

    for (i = 0U; i < (OS_U16)OS_MAX_TIMERS; i++) {
        Pool[i].Active     = false;
        Pool[i].Generation = 0U;
        Pool[i].NextFree   = (OS_I16)(i + 1);
        Pool[i].NextHsm    = -1;
        Pool[i].PrevHsm    = -1;
        Pool[i].NextWheel  = -1;
        Pool[i].PrevWheel  = -1;
    }
    Pool[OS_MAX_TIMERS - 1U].NextFree = -1;

    for (i = 0U; i < (OS_U16)OS_TIMER_WHEEL_SIZE; i++) {
        Wheel[i] = -1;
    }

    FreeHead    = 0;
    TickCounter = 0U;
    WdgEnabled  = false;
}

/* ------------------------------------------------------------------ */
OS_TimerHandle OS_TimerCreate(OS_Signal signal,
                              OS_U32 periodMs, bool periodic)
{
    OS_TimerHandle handle;
    OS_U16         idx;
    OS_Hsm        *me;

    Q_ASSERT(OS_HsmInDispatch());
    me = OS_HsmGetCurrent();
    Q_ASSERT(me != (OS_Hsm *)0);
    Q_ASSERT(me->Initialized);
    Q_ASSERT(periodMs > 0U);

    Port_CriticalEnter();

    /* Reject duplicate: walk only this HSM's active-timer list. */
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

    Pool[idx].Expiry     = TickCounter + periodMs;
    Pool[idx].Period     = periodic ? periodMs : 0U;
    Pool[idx].Signal     = signal;
    Pool[idx].Hook       = me;
    Pool[idx].OwnerState = me->State[OS_HsmGetDispatchDepth()];
    Pool[idx].Generation = Pool[idx].Generation + 1U;
    Pool[idx].Active     = true;

    /* Insert at head of per-HSM list – O(1). */
    Pool[idx].NextHsm = me->TimerHead;
    Pool[idx].PrevHsm = -1;
    if (me->TimerHead >= 0) {
        Pool[(OS_U16)me->TimerHead].PrevHsm = (OS_I16)idx;
    }
    me->TimerHead = (OS_I16)idx;

    /* Insert into timing-wheel slot – O(1). */
    WheelInsert(idx);

    handle.Index      = idx;
    handle.Generation = Pool[idx].Generation;

    Port_CriticalExit();

    return handle;
}

/* ------------------------------------------------------------------ */
void OS_TimerDelete(OS_TimerHandle handle)
{
    TimerBlock *t;

    Q_ASSERT(handle.Index < (OS_U16)OS_MAX_TIMERS);

    Port_CriticalEnter();

    t = &Pool[handle.Index];

    if (t->Active && (t->Generation == handle.Generation)) {
        /* Protection: only the owning state may delete. */
        Q_ASSERT(OS_HsmInDispatch());
        Q_ASSERT(t->Hook == OS_HsmGetCurrent());
        Q_ASSERT(t->OwnerState
                 == t->Hook->State[OS_HsmGetDispatchDepth()]);

        TimerFree(handle.Index);
    }

    Port_CriticalExit();
}

/* ------------------------------------------------------------------ */
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

    /* Inspect the single wheel slot for this tick – O(1). */
    slot = (OS_U16)(TickCounter & (OS_U32)OS_TIMER_WHEEL_MASK);
    cur  = Wheel[slot];

    while (cur >= 0) {
        next = Pool[(OS_U16)cur].NextWheel;

        if (TickLeq(Pool[(OS_U16)cur].Expiry, TickCounter)) {
            OS_InsertEventFromIsr(Pool[(OS_U16)cur].Signal,
                                  Pool[(OS_U16)cur].Hook);

            if (Pool[(OS_U16)cur].Period > 0U) {
                /* Periodic: move to the new wheel slot – O(1). */
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

    WatchdogTick();

    Port_CriticalExit();
}

/* ------------------------------------------------------------------ */
void OS_WatchdogInit(OS_U32 timeoutMs)
{
    Port_CriticalEnter();
    WdgTimeout = timeoutMs;
    WdgCounter = timeoutMs;
    WdgEnabled = true;
    Port_CriticalExit();

    Port_WatchdogInit(timeoutMs);
}

/* ------------------------------------------------------------------ */
void OS_WatchdogFeed(void)
{
    Port_CriticalEnter();
    WdgCounter = WdgTimeout;
    Port_CriticalExit();

    Port_WatchdogFeed();
}

/* ------------------------------------------------------------------ */
OS_U32 OS_GetTickCount(void)
{
    return TickCounter;
}
