/**
 * @file  OS_Config.h
 * @brief RTEF OS compile-time configuration constants.
 */
#ifndef OS_CONFIG_H
#define OS_CONFIG_H

/** @brief Maximum events in the queue (must be a power of two). */
#define OS_MAX_EVENTS        32U

/** @brief Bit-mask for O(1) circular-buffer indexing. */
#define OS_EVENT_MASK        (OS_MAX_EVENTS - 1U)

/** @brief Maximum number of simultaneous software timers. */
#define OS_MAX_TIMERS        16U

/** @brief Maximum nesting depth of an HSM (top + children). */
#define OS_HSM_MAX_DEPTH     4U

/** @brief Default software-watchdog timeout (ms). */
#define OS_WATCHDOG_TIMEOUT_MS  5000U

#endif /* OS_CONFIG_H */
