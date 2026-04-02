/**
 * @file  OS_Event.h
 * @brief Fixed-size event queue for the RTEF OS.
 */
#ifndef OS_EVENT_H
#define OS_EVENT_H

#include "OS_Types.h"

/**
 * @brief Initialise the event queue.  Call once at start-up.
 */
void OS_EventInit(void);

/**
 * @brief Insert an event into the queue (user API).
 *
 * Protection: while the OS is dispatching, only the currently executing
 * HSM may post events to itself.
 *
 * @param signal  Signal identifier (>= Q_USER).
 * @param hook    Target HSM instance.
 */
void OS_InsertEvent(OS_Signal signal, OS_Hsm *hook);

/**
 * @brief Insert an event from ISR / SysTick context.
 *
 * No HSM-ownership check is performed.  The caller **must** already
 * hold the critical-section lock.
 *
 * @param signal  Signal identifier.
 * @param hook    Target HSM instance.
 */
void OS_InsertEventFromIsr(OS_Signal signal, OS_Hsm *hook);

/**
 * @brief Dequeue and dispatch one event.  Called from the main loop.
 */
void OS_EventDispatch(void);

#endif /* OS_EVENT_H */
