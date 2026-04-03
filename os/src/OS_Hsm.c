/**
 * @file  OS_Hsm.c
 * @brief Hierarchical State Machine engine implementation.
 */
#include "OS_Hsm.h"
#include "OS_Timer.h"
#include "OS_Error.h"
#include "OS_Port.h"

/* ------------------------------------------------------------------ */
/*  Dispatch context (module-private)                                 */
/* ------------------------------------------------------------------ */

static OS_Hsm *CurrentHsm          = (OS_Hsm *)0;
static OS_U8   CurrentDispatchDepth = 0U;
static bool    Dispatching          = false;

/* ------------------------------------------------------------------ */
/*  Private helpers                                                   */
/* ------------------------------------------------------------------ */

/**
 * @brief Dispatch a system signal directly (no bubbling).
 */
static void DispatchDirect(OS_Hsm *me, OS_U8 depth, OS_Signal signal)
{
    OS_Event e;
    e.Signal = signal;
    (void)me->State[depth](me, &e);
}

/**
 * @brief Enter a state: dispatch Q_ENTRY then Q_INIT (recursive).
 */
static void EnterState(OS_Hsm *me, OS_U8 depth)
{
    CurrentDispatchDepth = depth;
    DispatchDirect(me, depth, (OS_Signal)Q_ENTRY);
    DispatchDirect(me, depth, (OS_Signal)Q_INIT);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

void OS_HsmInit(OS_Hsm *me, OS_StateHandler topState)
{
    OS_U8 i;

    Q_ASSERT(me != (OS_Hsm *)0);
    Q_ASSERT(topState != (OS_StateHandler)0);
    Q_ASSERT(!me->Initialized);

    me->Depth       = 0U;
    me->Initialized = true;
    me->TimerHead   = -1;
    me->State[0]    = topState;

    for (i = 1U; i < (OS_U8)OS_HSM_MAX_DEPTH; i++) {
        me->State[i] = (OS_StateHandler)0;
    }

    /* Set dispatch context so timers / child-init work during init. */
    CurrentHsm          = me;
    Dispatching          = true;
    CurrentDispatchDepth = 0U;

    EnterState(me, 0U);

    Dispatching = false;
    CurrentHsm  = (OS_Hsm *)0;
}

/* ------------------------------------------------------------------ */
void OS_HsmChildInit(OS_Hsm *me, OS_StateHandler childState)
{
    Q_ASSERT(Dispatching);
    Q_ASSERT(me == CurrentHsm);
    Q_ASSERT(childState != (OS_StateHandler)0);
    Q_ASSERT(me->Depth < ((OS_U8)OS_HSM_MAX_DEPTH - 1U));

    me->Depth++;
    me->State[me->Depth] = childState;

    EnterState(me, me->Depth);
}

/* ------------------------------------------------------------------ */
void OS_HsmTransition(OS_Hsm *me, OS_StateHandler target)
{
    OS_U8 transDepth;

    Q_ASSERT(Dispatching);
    Q_ASSERT(me == CurrentHsm);
    Q_ASSERT(target != (OS_StateHandler)0);

    transDepth = CurrentDispatchDepth;

    /* Exit every descendant, bottom-up. */
    while (me->Depth > transDepth) {
        CurrentDispatchDepth = me->Depth;
        DispatchDirect(me, me->Depth, (OS_Signal)Q_EXIT);
        OS_TimerDeleteByState();
        me->State[me->Depth] = (OS_StateHandler)0;
        me->Depth--;
    }

    /* Exit the current state at transDepth. */
    CurrentDispatchDepth = transDepth;
    DispatchDirect(me, transDepth, (OS_Signal)Q_EXIT);
    OS_TimerDeleteByState();

    /* Install and enter the target state. */
    me->State[transDepth] = target;
    CurrentDispatchDepth  = transDepth;

    EnterState(me, transDepth);
}

/* ------------------------------------------------------------------ */
void OS_HsmDispatch(OS_Hsm *me, OS_Signal signal)
{
    OS_Event e;
    OS_I8    d;

    Q_ASSERT(me != (OS_Hsm *)0);
    Q_ASSERT(me->Initialized);

    e.Signal = signal;

    CurrentHsm  = me;
    Dispatching = true;

    /* Try from the deepest active state upward (event bubbling). */
    for (d = (OS_I8)me->Depth; d >= 0; d--) {
        CurrentDispatchDepth = (OS_U8)d;
        if (me->State[(OS_U8)d](me, &e) == OS_HANDLED) {
            break;
        }
    }

    Dispatching = false;
    CurrentHsm  = (OS_Hsm *)0;
}

/* ------------------------------------------------------------------ */
/*  Context queries                                                   */
/* ------------------------------------------------------------------ */

OS_Hsm *OS_HsmGetCurrent(void)
{
    return CurrentHsm;
}

bool OS_HsmInDispatch(void)
{
    return Dispatching;
}

OS_U8 OS_HsmGetDispatchDepth(void)
{
    return CurrentDispatchDepth;
}
