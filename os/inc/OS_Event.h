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
 * @brief Insert an event into the queue.
 *
 * Callable from any context: task / HSM state handler / ISR / SysTick.
 * Acquires the critical-section lock internally; nests safely when the
 * caller already holds it (port lock is reentrant).
 *
 * @param signal  Signal identifier (>= Q_USER for user signals).
 * @param hook    Target HSM instance.
 */
void OS_InsertEvent(OS_Signal signal, OS_Hsm *hook);

/**
 * @brief Dequeue and dispatch one event.  Called from the main loop.
 */
void OS_EventDispatch(void);

#endif /* OS_EVENT_H */
