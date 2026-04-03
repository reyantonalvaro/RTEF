/**
 * @file  OS_Port.h
 * @brief Hardware-abstraction interface that every port must implement.
 *
 * The generic OS sources call these functions.  Each target platform
 * provides its own implementation in the corresponding port/ folder.
 */
#ifndef OS_PORT_H
#define OS_PORT_H

#include "OS_Types.h"

/**
 * @brief One-time hardware / platform initialisation.
 */
void Port_Init(void);

/**
 * @brief Enter a critical section (disable interrupts / acquire lock).
 */
void Port_CriticalEnter(void);

/**
 * @brief Leave a critical section (enable interrupts / release lock).
 */
void Port_CriticalExit(void);

/**
 * @brief Log a fatal OS error to the output device.
 *
 * @param id   Numeric error identifier.
 * @param desc Human-readable description (stringified assertion).
 * @param file Source file where the error occurred.
 * @param line Source line number.
 */
void Port_ErrorLog(OS_U32 id, char const *desc,
                   char const *file, OS_U32 line);

/**
 * @brief Halt the system after a fatal error.
 */
void Port_SystemHalt(void);

/**
 * @brief Start the periodic system-tick source.
 *
 * The tick period is defined by OS_TICK_PERIOD_MS (see OS_Config.h).
 */
void Port_SysTickStart(void);

/**
 * @brief Stop the system-tick source.
 */
void Port_SysTickStop(void);

/**
 * @brief Initialise the hardware watchdog.
 * @param timeoutTicks Timeout in OS ticks (see OS_TICK_PERIOD_MS).
 */
void Port_WatchdogInit(OS_U32 timeoutTicks);

/**
 * @brief Feed / kick the hardware watchdog.
 */
void Port_WatchdogFeed(void);

#endif /* OS_PORT_H */
