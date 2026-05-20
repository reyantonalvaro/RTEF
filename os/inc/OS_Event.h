/**
 * @file  OS_Event.h
 * @brief Fixed-size event queue for the RTEF OS.
 */
#ifndef OS_EVENT_H
#define OS_EVENT_H

#include "OS_Types.h"

/**
 * @brief Insert an event into the queue.
 *
 * Callable from any context: task / HSM state handler / ISR / SysTick.
 * Acquires the critical-section lock internally; nests safely when the
 * caller already holds it (port lock is reentrant).
 *
 * @param signal  Signal identifier (>= Q_USER for user signals).
 * @param param   Optional 32-bit payload (use 0 when unused).
 * @param hook    Target HSM instance.
 */
void OS_InsertEvent(OS_Signal signal, OS_U32 param, OS_Hsm *hook);

/**
 * @brief Dequeue and dispatch one event. Called from the main loop.
 *
 * @return true  if an event was dispatched,
 *         false if the queue was empty (caller may sleep / WFI).
 */
bool OS_EventDispatch(void);

/**
 * @brief Highest queue occupancy reached since boot.
 *
 * Use to size OS_MAX_EVENTS empirically. Read at any time; updated
 * inside the queue's critical section.
 */
OS_U16 OS_EventQueueHighWater(void);

#endif /* OS_EVENT_H */
