/**
 * @file  OS_Watchdog.h
 * @brief Software watchdog for the RTEF OS.
 *
 * The watchdog is a separate module from the timer system.  It is
 * advanced by one tick each time OS_SysTick() is called and fires a
 * Q_ASSERT (fatal halt) if OS_WatchdogFeed() is not called in time.
 */
#ifndef OS_WATCHDOG_H
#define OS_WATCHDOG_H

#include "OS_Types.h"

/**
 * @brief Initialise and start the software watchdog.
 *
 * @param timeoutTicks  Watchdog timeout in OS ticks.
 *                      Must be >= OS_WATCHDOG_MIN_TICKS (see OS_Config.h).
 *                      See OS_TICK_PERIOD_MS for tick duration.
 */
void OS_WatchdogInit(OS_U32 timeoutTicks);

/**
 * @brief Feed the software watchdog, resetting its countdown counter.
 *
 * Call this regularly from the main loop or a watchdog-kick state to
 * prevent the system from halting.
 */
void OS_WatchdogFeed(void);

#endif /* OS_WATCHDOG_H */
