/**
 * @file  OS_Event.c
 * @brief Fixed-size ring-buffer event queue – O(1) insert and dispatch.
 */
#include "OS_Event.h"
#include "OS_Error.h"
#include "OS_Hsm.h"
#include "OS_Config.h"
#include "OS_Port.h"

_Static_assert((OS_MAX_EVENTS & (OS_MAX_EVENTS - 1U)) == 0U,
               "OS_MAX_EVENTS must be a power of two");
_Static_assert(OS_MAX_EVENTS >= 2U,
               "OS_MAX_EVENTS must be at least 2");

typedef struct {
    OS_Signal  Signal;
    OS_U32     Param;
    OS_Hsm    *Hook;
} EventEntry;

/* Static storage: BSS-zeroed at startup (Head = Tail = Count = 0). */
static EventEntry Queue[OS_MAX_EVENTS];
static OS_U16     Head;
static OS_U16     Tail;
static OS_U16     Count;
static OS_U16     CountMax;

void OS_InsertEvent(OS_Signal signal, OS_U32 param, OS_Hsm *hook)
{
    Q_ASSERT(hook != (OS_Hsm *)0);
    Q_ASSERT(hook->Initialized);
    Q_ASSERT(signal != (OS_Signal)Q_EMPTY);

    Port_CriticalEnter();

    Q_ASSERT(Count < (OS_U16)OS_MAX_EVENTS);

    Queue[Head].Signal = signal;
    Queue[Head].Param  = param;
    Queue[Head].Hook   = hook;
    Head  = (Head + 1U) & (OS_U16)OS_EVENT_MASK;
    Count++;
    if (Count > CountMax) {
        CountMax = Count;
    }

    Port_CriticalExit();
}

bool OS_EventDispatch(void)
{
    OS_Signal signal;
    OS_U32    param;
    OS_Hsm   *hook;

    Port_CriticalEnter();
    if (Count == 0U) {
        Port_CriticalExit();
        return false;
    }
    signal = Queue[Tail].Signal;
    param  = Queue[Tail].Param;
    hook   = Queue[Tail].Hook;
    Tail   = (Tail + 1U) & (OS_U16)OS_EVENT_MASK;
    Count--;
    Port_CriticalExit();

    OS_HsmDispatch(hook, signal, param);
    return true;
}

OS_U16 OS_EventQueueHighWater(void)
{
    OS_U16 v;
    Port_CriticalEnter();
    v = CountMax;
    Port_CriticalExit();
    return v;
}
