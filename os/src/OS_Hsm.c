/**
 * @file  OS_Hsm.c
 * @brief Hierarchical State Machine engine implementation.
 */
#include "OS_Hsm.h"
#include "OS_Timer_Private.h"
#include "OS_Error.h"
#include "OS_Port.h"

static OS_Hsm *CurrentHsm           = (OS_Hsm *)0;
static OS_U8   CurrentDispatchDepth = 0U;
static bool    Dispatching          = false;

/* Dispatch one system signal (Q_ENTRY/Q_EXIT/Q_INIT) at a given depth.
 * System signals never carry a payload, so Param is forced to 0. */
static void DispatchSysSignal(OS_Hsm *me, OS_U8 depth, OS_Signal signal)
{
    OS_Event const e = { signal, 0U };
    CurrentDispatchDepth = depth;
    (void)me->State[depth](me, &e);
}

/* Q_ENTRY then Q_INIT — may recurse into children via OS_HsmChildInit. */
static void EnterState(OS_Hsm *me, OS_U8 depth)
{
    DispatchSysSignal(me, depth, (OS_Signal)Q_ENTRY);
    DispatchSysSignal(me, depth, (OS_Signal)Q_INIT);
}

/* Q_EXIT plus auto-deletion of timers owned by the state being left. */
static void ExitState(OS_Hsm *me, OS_U8 depth)
{
    DispatchSysSignal(me, depth, (OS_Signal)Q_EXIT);
    OS_TimerDeleteByState();
}

void OS_HsmInit(OS_Hsm *me, OS_StateHandler topState)
{
    OS_U8 i;

    Q_ASSERT(me != (OS_Hsm *)0);
    Q_ASSERT(topState != (OS_StateHandler)0);
    Q_ASSERT(!me->Initialized);

    me->Depth       = 0U;
    me->Initialized = true;
    me->TimerHead   = -1;
    me->TimerCount  = 0U;
    me->State[0]    = topState;
    for (i = 1U; i < (OS_U8)OS_HSM_MAX_DEPTH; i++) {
        me->State[i] = (OS_StateHandler)0;
    }

    /* Establish dispatch context so timers / child-init work during init. */
    CurrentHsm  = me;
    Dispatching = true;
    EnterState(me, 0U);
    Dispatching = false;
    CurrentHsm  = (OS_Hsm *)0;
}

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

void OS_HsmTransition(OS_Hsm *me, OS_StateHandler target)
{
    OS_U8 const transDepth = CurrentDispatchDepth;

    Q_ASSERT(Dispatching);
    Q_ASSERT(me == CurrentHsm);
    Q_ASSERT(target != (OS_StateHandler)0);

    /* Exit every descendant bottom-up, then exit the transitioning state. */
    while (me->Depth > transDepth) {
        ExitState(me, me->Depth);
        me->State[me->Depth] = (OS_StateHandler)0;
        me->Depth--;
    }
    ExitState(me, transDepth);

    /* Install and enter the target sibling. */
    me->State[transDepth] = target;
    EnterState(me, transDepth);
}

/* Param is plumbed through from the event queue so the handler sees
 * exactly what the producer posted (no payload-via-globals). */
void OS_HsmDispatch(OS_Hsm *me, OS_Signal signal, OS_U32 param)
{
    OS_Event const e = { signal, param };
    OS_I8          d;

    Q_ASSERT(me != (OS_Hsm *)0);
    Q_ASSERT(me->Initialized);

    CurrentHsm  = me;
    Dispatching = true;

    /* Event bubbling: try the deepest state first, then parents. */
    for (d = (OS_I8)me->Depth; d >= 0; d--) {
        CurrentDispatchDepth = (OS_U8)d;
        if (me->State[(OS_U8)d](me, &e) == OS_HANDLED) {
            break;
        }
    }

    Dispatching = false;
    CurrentHsm  = (OS_Hsm *)0;
}

OS_Hsm *OS_HsmGetCurrent(void)         { return CurrentHsm; }
bool    OS_HsmInDispatch(void)         { return Dispatching; }
OS_U8   OS_HsmGetDispatchDepth(void)   { return CurrentDispatchDepth; }
