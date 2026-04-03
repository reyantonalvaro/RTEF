/**
 * @file  OS_Timer.c
 * @brief Software-timer pool, SysTick handler and watchdog – all O(1).
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
    OS_U32          Remaining;    /**< Ticks until next expiry.       */
    OS_U32          Period;       /**< Reload value (0 = one-shot).   */
    OS_Signal       Signal;       /**< Signal posted on expiry.       */
    OS_Hsm         *Hook;         /**< Owning HSM.                    */
    OS_StateHandler OwnerState;   /**< State that created this timer. */
    OS_U16          Generation;   /**< Stale-handle detection.        */
    OS_I16          NextFree;     /**< Free-list link (-1 = end).     */
    OS_I16          NextHsm;      /**< Per-HSM list forward link.     */
    OS_I16          PrevHsm;      /**< Per-HSM list backward link.    */
    bool            Active;       /**< true while timer is running.   */
} TimerBlock;

/* ------------------------------------------------------------------ */
/*  Module-private state                                              */
/* ------------------------------------------------------------------ */

static TimerBlock Pool[OS_MAX_TIMERS];
static OS_I16     FreeHead;

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
 * @brief Return a pool slot to the free list.
 * @param idx  Slot index.
 */
static void TimerFree(OS_U16 idx)
{
    /* Unlink from per-HSM doubly-linked list. */
    if (Pool[idx].PrevHsm >= 0) {
        Pool[(OS_U16)Pool[idx].PrevHsm].NextHsm = Pool[idx].NextHsm;
    } else {
        Pool[idx].Hook->TimerHead = Pool[idx].NextHsm;
    }
    if (Pool[idx].NextHsm >= 0) {
        Pool[(OS_U16)Pool[idx].NextHsm].PrevHsm = Pool[idx].PrevHsm;
    }

    Pool[idx].Active   = false;
    Pool[idx].NextHsm  = -1;
    Pool[idx].PrevHsm  = -1;
    Pool[idx].NextFree = FreeHead;
    FreeHead           = (OS_I16)idx;
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
    }
    Pool[OS_MAX_TIMERS - 1U].NextFree = -1;

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

    Pool[idx].Remaining  = periodMs;
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
void OS_TimerDeleteByState(OS_Hsm *hook, OS_StateHandler state)
{
    OS_I16 cur;
    OS_I16 next;

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
    OS_U16 i;

    Port_CriticalEnter();

    TickCounter++;

    for (i = 0U; i < (OS_U16)OS_MAX_TIMERS; i++) {
        if (Pool[i].Active) {
            if (Pool[i].Remaining > 0U) {
                Pool[i].Remaining--;
            }
            if (Pool[i].Remaining == 0U) {
                OS_InsertEventFromIsr(Pool[i].Signal, Pool[i].Hook);

                if (Pool[i].Period > 0U) {
                    Pool[i].Remaining = Pool[i].Period;
                } else {
                    TimerFree(i);
                }
            }
        }
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
