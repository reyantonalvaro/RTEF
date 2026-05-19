/**
 * @file  OS_Event.c
 * @brief Fixed-size ring-buffer event queue – O(1) insert and dispatch.
 */
#include "OS_Event.h"
#include "OS_Error.h"
#include "OS_Hsm.h"
#include "OS_Config.h"
#include "OS_Port.h"

typedef struct {
    OS_Signal  Signal;
    OS_Hsm    *Hook;
} EventEntry;

static EventEntry Queue[OS_MAX_EVENTS];
static OS_U16     Head;
static OS_U16     Tail;
static OS_U16     Count;

/* Ring-buffer write. Caller MUST hold the critical-section lock. */
static void EnqueueLocked(OS_Signal signal, OS_Hsm *hook)
{
    Q_ASSERT(hook != (OS_Hsm *)0);
    Q_ASSERT(hook->Initialized);
    Q_ASSERT(Count < (OS_U16)OS_MAX_EVENTS);

    Queue[Head].Signal = signal;
    Queue[Head].Hook   = hook;
    Head  = (Head + 1U) & (OS_U16)OS_EVENT_MASK;
    Count++;
}

void OS_EventInit(void)
{
    Head  = 0U;
    Tail  = 0U;
    Count = 0U;
}

void OS_InsertEvent(OS_Signal signal, OS_Hsm *hook)
{
    Port_CriticalEnter();

    /* Protection: during dispatch only the running HSM may post to itself. */
    if (OS_HsmInDispatch()) {
        Q_ASSERT(hook == OS_HsmGetCurrent());
    }
    EnqueueLocked(signal, hook);

    Port_CriticalExit();
}

void OS_InsertEventFromIsr(OS_Signal signal, OS_Hsm *hook)
{
    /* Caller (OS_SysTick) already holds the critical-section lock. */
    EnqueueLocked(signal, hook);
}

void OS_EventDispatch(void)
{
    OS_Signal signal;
    OS_Hsm   *hook;

    Port_CriticalEnter();
    if (Count == 0U) {
        Port_CriticalExit();
        return;
    }
    signal = Queue[Tail].Signal;
    hook   = Queue[Tail].Hook;
    Tail   = (Tail + 1U) & (OS_U16)OS_EVENT_MASK;
    Count--;
    Port_CriticalExit();

    OS_HsmDispatch(hook, signal);
}
