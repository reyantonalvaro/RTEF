/**
 * @file  OS_Watchdog_Private.h
 * @brief OS-internal watchdog tick — NOT part of the public interface.
 *
 * Include this header ONLY from OS source files (os/src/).
 */
#ifndef OS_WATCHDOG_PRIVATE_H
#define OS_WATCHDOG_PRIVATE_H

/**
 * @brief Advance the watchdog counter by one tick.
 *
 * Called from OS_SysTick() — the highest-priority ISR.
 * Triggers Q_ASSERT if the counter reaches zero (watchdog expired).
 */
void OS_WatchdogTick(void);

#endif /* OS_WATCHDOG_PRIVATE_H */
