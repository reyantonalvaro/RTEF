/**
 * @file  OS_Timer.c
 * @brief Software-timer pool with timing wheel — all O(1) operations.
 *
 * Architecture:
 *   - A pool of TimerBlocks (size = OS_TIMER_WHEEL_SIZE).
 *   - A timing wheel indexed by (Expiry & WHEEL_MASK).  Each timer carries a
 *     Round counter (full rotations remaining) so periods up to 2^32 ticks
 *     work with O(1) per-tick cost: only one slot is inspected per tick.
 *   - Per-HSM doubly-linked list so OS_TimerDeleteByState() is O(m).
 *
 * Round formula (unsigned, no overflow):
 *     delta = Expiry - TickCounter      // always >= 1 (periodTicks > 0)
 *     Round = (delta - 1) >> WHEEL_BITS
 *
 * OS_SysTick only enqueues events — it never dispatches. The tick ISR is
 * therefore bounded by the number of timers in one wheel slot.
 *
 * Stale-handle guard: active timers have Generation >= 1, so the
 * OS_TIMER_INVALID sentinel {0xFFFF, 0} can never collide.
 */
#include "OS_Timer.h"
#include "OS_Timer_Private.h"
#include "OS_Watchdog_Private.h"
#include "OS_Event.h"
#include "OS_Error.h"
#include "OS_Hsm.h"
#include "OS_Config.h"
#include "OS_Port.h"

_Static_assert((OS_TIMER_WHEEL_SIZE & (OS_TIMER_WHEEL_SIZE - 1U)) == 0U,
               "OS_TIMER_WHEEL_SIZE must be a power of two");
_Static_assert(OS_TIMER_WHEEL_SIZE >= 2U,
               "OS_TIMER_WHEEL_SIZE must be at least 2");
_Static_assert(OS_TIMER_WHEEL_SIZE <= 32767U,
               "OS_TIMER_WHEEL_SIZE must fit in OS_I16");
_Static_assert(OS_TIMER_MAX_PER_HSM >= 1U
            && OS_TIMER_MAX_PER_HSM <= OS_TIMER_WHEEL_SIZE
            && OS_TIMER_MAX_PER_HSM <= 255U,
               "OS_TIMER_MAX_PER_HSM out of range");

typedef struct {
    OS_U32          Expiry;       /**< Absolute tick of next expiry.   */
    OS_U32          Period;       /**< Reload value (0 = one-shot).    */
    OS_U32          Round;        /**< Full wheel rotations remaining. */
    OS_Signal       Signal;       /**< Signal posted on expiry.        */
    OS_Hsm         *Hook;         /**< Owning HSM.                     */
    OS_StateHandler OwnerState;   /**< State that created this timer.  */
    OS_U16          Generation;   /**< Stale-handle guard (>= 1).      */
    OS_I16          NextFree;     /**< Free-list link (-1 = end).      */
    OS_I16          NextHsm;      /**< Per-HSM list forward link.      */
    OS_I16          PrevHsm;      /**< Per-HSM list backward link.     */
    OS_I16          NextWheel;    /**< Wheel-slot list forward link.   */
    OS_I16          PrevWheel;    /**< Wheel-slot list backward link.  */
    bool            Active;       /**< true while the timer is running.*/
} TimerBlock;

static TimerBlock      Pool[OS_TIMER_WHEEL_SIZE];
static OS_I16          Wheel[OS_TIMER_WHEEL_SIZE];
static OS_I16          FreeHead;
static volatile OS_U32 TickCounter;

/* ------------------------------------------------------------------ */
/*  Wheel-slot list maintenance                                        */
/* ------------------------------------------------------------------ */

static void WheelInsert(OS_U16 idx)
{
    TimerBlock *const t    = &Pool[idx];
    OS_U32      const delta = t->Expiry - TickCounter - 1U;
    OS_U16      const slot  = (OS_U16)(t->Expiry & (OS_U32)OS_TIMER_WHEEL_MASK);

    t->Round     = delta >> (OS_U32)OS_TIMER_WHEEL_BITS;
    t->PrevWheel = -1;
    t->NextWheel = Wheel[slot];
    if (Wheel[slot] >= 0) {
        Pool[(OS_U16)Wheel[slot]].PrevWheel = (OS_I16)idx;
    }
    Wheel[slot] = (OS_I16)idx;
}

static void WheelRemove(OS_U16 idx)
{
    TimerBlock *const t    = &Pool[idx];
    OS_U16      const slot = (OS_U16)(t->Expiry & (OS_U32)OS_TIMER_WHEEL_MASK);

    if (t->PrevWheel >= 0) {
        Pool[(OS_U16)t->PrevWheel].NextWheel = t->NextWheel;
    } else {
        Wheel[slot] = t->NextWheel;
    }
    if (t->NextWheel >= 0) {
        Pool[(OS_U16)t->NextWheel].PrevWheel = t->PrevWheel;
    }
    t->NextWheel = -1;
    t->PrevWheel = -1;
}

/* ------------------------------------------------------------------ */
/*  Per-HSM list + pool free-list maintenance                          */
/* ------------------------------------------------------------------ */

static void TimerFree(OS_U16 idx)
{
    TimerBlock *const t = &Pool[idx];

    WheelRemove(idx);

    if (t->PrevHsm >= 0) {
        Pool[(OS_U16)t->PrevHsm].NextHsm = t->NextHsm;
    } else {
        t->Hook->TimerHead = t->NextHsm;
    }
    if (t->NextHsm >= 0) {
        Pool[(OS_U16)t->NextHsm].PrevHsm = t->PrevHsm;
    }
    t->Hook->TimerCount--;

    /* Only the fields needed for free-list sanity. Generation is preserved
     * so old handles cannot be revived after the slot is reused. */
    t->Active   = false;
    t->Hook     = (OS_Hsm *)0;
    t->NextFree = FreeHead;
    FreeHead    = (OS_I16)idx;
}

/* ------------------------------------------------------------------ */
/*  Handle validation (shared by Delete and Restart)                   */
/* ------------------------------------------------------------------ */

/* Returns the timer if the handle is alive AND owned by the calling state;
 * returns NULL if the handle is stale.  Asserts on ownership mismatch. */
static TimerBlock *OwnedTimerFromHandle(OS_TimerHandle handle)
{
    TimerBlock *t;

    if (handle.Index >= (OS_U16)OS_TIMER_WHEEL_SIZE) {
        return (TimerBlock *)0;
    }
    t = &Pool[handle.Index];
    if (!t->Active || (t->Generation != handle.Generation)) {
        return (TimerBlock *)0;
    }

    Q_ASSERT(OS_HsmInDispatch());
    Q_ASSERT(t->Hook == OS_HsmGetCurrent());
    Q_ASSERT(t->OwnerState == t->Hook->State[OS_HsmGetDispatchDepth()]);
    return t;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

void OS_TimerInit(void)
{
    OS_U16 i;

    for (i = 0U; i < (OS_U16)OS_TIMER_WHEEL_SIZE; i++) {
        Pool[i].Active     = false;
        Pool[i].Generation = 0U;
        Pool[i].Hook       = (OS_Hsm *)0;
        Pool[i].NextHsm    = -1;
        Pool[i].PrevHsm    = -1;
        Pool[i].NextWheel  = -1;
        Pool[i].PrevWheel  = -1;
        Pool[i].NextFree   = (OS_I16)(i + 1U);
        Wheel[i]           = -1;
    }
    Pool[OS_TIMER_WHEEL_SIZE - 1U].NextFree = -1;

    FreeHead    = 0;
    TickCounter = 0U;
}

OS_TimerHandle OS_TimerCreate(OS_Signal signal,
                              OS_U32 periodTicks, bool periodic)
{
    OS_TimerHandle  handle;
    OS_Hsm   *const me = OS_HsmGetCurrent();
    TimerBlock     *t;
    OS_U16          idx;
    OS_U16          newGen;
    OS_I16          cur;

    Q_ASSERT(OS_HsmInDispatch());
    Q_ASSERT(me != (OS_Hsm *)0);
    Q_ASSERT(me->Initialized);
    Q_ASSERT(periodTicks > 0U);

    Port_CriticalEnter();

    Q_ASSERT(me->TimerCount < (OS_U8)OS_TIMER_MAX_PER_HSM);

    /* Duplicate-signal check — walk this HSM's timers (bounded). */
    for (cur = me->TimerHead; cur >= 0; cur = Pool[(OS_U16)cur].NextHsm) {
        Q_ASSERT(Pool[(OS_U16)cur].Signal != signal);
    }

    Q_ASSERT(FreeHead >= 0);
    idx      = (OS_U16)FreeHead;
    t        = &Pool[idx];
    FreeHead = t->NextFree;

    /* Generation always >= 1; skip 0 on wrap so OS_TIMER_INVALID stays unique. */
    newGen = (OS_U16)(t->Generation + 1U);
    if (newGen == 0U) {
        newGen = 1U;
    }
    t->Generation = newGen;
    t->Expiry     = TickCounter + periodTicks;
    t->Period     = periodic ? periodTicks : 0U;
    t->Signal     = signal;
    t->Hook       = me;
    t->OwnerState = me->State[OS_HsmGetDispatchDepth()];
    t->Active     = true;

    /* Insert at head of the per-HSM list. */
    t->NextHsm = me->TimerHead;
    t->PrevHsm = -1;
    if (me->TimerHead >= 0) {
        Pool[(OS_U16)me->TimerHead].PrevHsm = (OS_I16)idx;
    }
    me->TimerHead = (OS_I16)idx;
    me->TimerCount++;

    WheelInsert(idx);

    handle.Index      = idx;
    handle.Generation = newGen;

    Port_CriticalExit();
    return handle;
}

bool OS_TimerDelete(OS_TimerHandle handle)
{
    TimerBlock *t;
    bool        ok;

    Port_CriticalEnter();
    t = OwnedTimerFromHandle(handle);
    if (t != (TimerBlock *)0) {
        TimerFree(handle.Index);
        ok = true;
    } else {
        ok = false;
    }
    Port_CriticalExit();
    return ok;
}

bool OS_TimerRestart(OS_TimerHandle handle, OS_U32 newPeriodTicks)
{
    TimerBlock *t;
    bool        ok;

    Q_ASSERT(newPeriodTicks > 0U);

    Port_CriticalEnter();
    t = OwnedTimerFromHandle(handle);
    if (t != (TimerBlock *)0) {
        WheelRemove(handle.Index);
        t->Expiry = TickCounter + newPeriodTicks;
        if (t->Period > 0U) {        /* Periodic: update reload too. */
            t->Period = newPeriodTicks;
        }
        WheelInsert(handle.Index);
        ok = true;
    } else {
        ok = false;
    }
    Port_CriticalExit();
    return ok;
}

/* OS-internal — declared in OS_Timer_Private.h, not in the public header. */
void OS_TimerDeleteByState(void)
{
    OS_Hsm   *const       hook  = OS_HsmGetCurrent();
    OS_StateHandler const state = hook->State[OS_HsmGetDispatchDepth()];
    OS_I16                cur;
    OS_I16                next;

    Port_CriticalEnter();
    for (cur = hook->TimerHead; cur >= 0; cur = next) {
        next = Pool[(OS_U16)cur].NextHsm;
        if (Pool[(OS_U16)cur].OwnerState == state) {
            TimerFree((OS_U16)cur);
        }
    }
    Port_CriticalExit();
}

void OS_SysTick(void)
{
    OS_U16 slot;
    OS_I16 cur;
    OS_I16 next;

    Port_CriticalEnter();
    TickCounter++;

    /* Inspect one wheel slot per tick — bounded by the slot's chain length. */
    slot = (OS_U16)(TickCounter & (OS_U32)OS_TIMER_WHEEL_MASK);
    for (cur = Wheel[slot]; cur >= 0; cur = next) {
        TimerBlock *const t = &Pool[(OS_U16)cur];
        next = t->NextWheel;

        if (t->Round > 0U) {
            t->Round--;
        } else {
            OS_InsertEvent(t->Signal, 0U, t->Hook);
            if (t->Period > 0U) {
                WheelRemove((OS_U16)cur);
                t->Expiry += t->Period;
                WheelInsert((OS_U16)cur);
            } else {
                TimerFree((OS_U16)cur);
            }
        }
    }

    OS_WatchdogTick();
    Port_CriticalExit();
}

OS_U32 OS_GetTickCount(void)
{
    OS_U32 count;
    /* Critical section so the read is atomic on 8/16-bit platforms. */
    Port_CriticalEnter();
    count = TickCounter;
    Port_CriticalExit();
    return count;
}
