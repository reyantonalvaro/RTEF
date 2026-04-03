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

/** @brief Timing-wheel width in bits (slots = 2^bits, must be power of two). */
#define OS_TIMER_WHEEL_BITS  8U

/** @brief Number of timing-wheel slots. */
#define OS_TIMER_WHEEL_SIZE  (1U << OS_TIMER_WHEEL_BITS)

/** @brief Bitmask for O(1) wheel-slot indexing. */
#define OS_TIMER_WHEEL_MASK  (OS_TIMER_WHEEL_SIZE - 1U)

/** @brief Maximum nesting depth of an HSM (top + children). */
#define OS_HSM_MAX_DEPTH     4U

/** @brief Default software-watchdog timeout (ms). */
#define OS_WATCHDOG_TIMEOUT_MS  5000U

#endif /* OS_CONFIG_H */
