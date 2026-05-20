/**
 * @file  OS_Event_Private.h
 * @brief OS-internal event API — NOT part of the public interface.
 *
 * Include this header ONLY from OS source files (os/src/).
 * User application code must never include it directly.
 */
#ifndef OS_EVENT_PRIVATE_H
#define OS_EVENT_PRIVATE_H

#include "OS_Types.h"

/**
 * @brief Insert an event into the queue targeting an explicit HSM.
 *
 * Reserved for OS-internal producers (e.g. the timer wheel firing from
 * SysTick) where there is no current dispatch context but the event is
 * still a deferred self-post on behalf of the HSM that armed the timer.
 *
 * User code MUST use the parameter-less OS_InsertEvent instead, which
 * resolves the target via OS_HsmGetCurrent() and therefore cannot post
 * to a foreign HSM.
 */
void OS_InsertEventForHsm(OS_Signal signal, OS_U32 param, OS_Hsm *hook);

#endif /* OS_EVENT_PRIVATE_H */
