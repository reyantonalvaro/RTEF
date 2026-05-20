/**
 * @file  OS_Event.h
 * @brief Fixed-size event queue for the RTEF OS.
 *
 * Public API kept intentionally minimal: insert and dispatch.
 */
#ifndef OS_EVENT_H
#define OS_EVENT_H

#include "OS_Types.h"

/**
 * @brief Insert an event into the queue, targeting the calling HSM.
 *
 * The target HSM is resolved internally via OS_HsmGetCurrent(); there
 * is no hook parameter. This enforces, by construction, that an HSM
 * can only post events to itself — no HSM can inject events into a
 * foreign HSM. Callable only from inside an HSM dispatch context
 * (state handler or init); asserts otherwise.
 *
 * Acquires the critical-section lock internally; nests safely when the
 * caller already holds it (port lock is reentrant).
 *
 * @param signal  Signal identifier (>= Q_USER for user signals).
 * @param param   32-bit payload carried with the event. Pass 0U when
 *                the receiving handler does not need a payload.
 */
void OS_InsertEvent(OS_Signal signal, OS_U32 param);

/**
 * @brief Dequeue and dispatch one event. Called from the main loop.
 *
 * @return true  if an event was dispatched,
 *         false if the queue was empty.
 *
 * The bool return lets the main loop drain the queue and then sleep
 * (WFI / nanosleep) instead of busy-polling, which is the canonical
 * event-driven idle pattern.
 */
bool OS_EventDispatch(void);

#endif /* OS_EVENT_H */
