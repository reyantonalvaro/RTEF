/**
 * @file  OS_Event.c
 * @brief Fixed-size ring-buffer event queue – O(1) insert and dispatch.
 */
#include "OS_Event.h"
#include "OS_Error.h"
#include "OS_Hsm.h"
#include "OS_Config.h"
#include "OS_Port.h"

/* The mask-based indexing (Head/Tail & OS_EVENT_MASK) only works if the
 * queue size is a power of two. Catch a bad OS_MAX_EVENTS at compile
 * time instead of corrupting the buffer silently at runtime. */
_Static_assert((OS_MAX_EVENTS & (OS_MAX_EVENTS - 1U)) == 0U,
               "OS_MAX_EVENTS must be a power of two");
_Static_assert(OS_MAX_EVENTS >= 2U,
               "OS_MAX_EVENTS must be at least 2");

typedef struct {
    OS_Signal  Signal;
    OS_U32     Param;  /* payload carried with the event */
    OS_Hsm    *Hook;
} EventEntry;

/* Static storage: BSS-zeroed at startup (Head = Tail = Count = 0). */
static EventEntry Queue[OS_MAX_EVENTS];
static OS_U16     Head;
static OS_U16     Tail;
static OS_U16     Count;

/* Design decision: NO high-water-mark / occupancy metric is tracked.
 * Sizing OS_MAX_EVENTS is handled at design time, not by runtime
 * telemetry. Keeping the module to the strict minimum (insert +
 * dispatch state only) is an explicit requirement. */

void OS_InsertEvent(OS_Signal signal, OS_U32 param, OS_Hsm *hook)
{
    /* Hook validation does not touch queue state, so do it before
     * disabling interrupts — keeps the critical section short and
     * therefore keeps worst-case interrupt latency low. */
    Q_ASSERT(hook != (OS_Hsm *)0);
    Q_ASSERT(hook->Initialized);

    /* Q_EMPTY (0) is reserved in OS_Types.h as "no signal". Reject it
     * here so the queue invariant matches its declared contract. */
    Q_ASSERT(signal != (OS_Signal)Q_EMPTY);

    Port_CriticalEnter();

    Q_ASSERT(Count < (OS_U16)OS_MAX_EVENTS);

    Queue[Head].Signal = signal;
    Queue[Head].Param  = param;
    Queue[Head].Hook   = hook;
    Head  = (Head + 1U) & (OS_U16)OS_EVENT_MASK;
    Count++;

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
        /* Empty queue: signal the caller so the main loop can sleep
         * (WFI / nanosleep) instead of spinning. */
        return false;
    }
    signal = Queue[Tail].Signal;
    param  = Queue[Tail].Param;
    hook   = Queue[Tail].Hook;
    Tail   = (Tail + 1U) & (OS_U16)OS_EVENT_MASK;
    Count--;
    /* Release the lock BEFORE invoking the handler. The handler may
     * post new events, run for a long time, or recurse — none of that
     * should happen with interrupts disabled. */
    Port_CriticalExit();

    OS_HsmDispatch(hook, signal, param);
    return true;
}
