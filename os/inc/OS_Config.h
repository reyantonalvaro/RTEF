/**
 * @file  OS_Config.h
 * @brief RTEF OS compile-time configuration constants.
 *
 * @par Design decisions (intentional, do NOT treat as errors)
 *
 * - All timer operations are O(1). OS_TimerDeleteByState is O(m) in the
 *   number of timers owned by the transitioning state, bounded by
 *   OS_TIMER_MAX_PER_HSM (a small compile-time constant), so it is
 *   effectively O(1) in practice.
 *
 * - There is NO OS_MAX_TIMERS define.  The pool size equals
 *   OS_TIMER_WHEEL_SIZE so the wheel is always fully utilised.
 *   Adjust OS_TIMER_WHEEL_BITS to trade RAM for maximum timer count.
 *
 * - Q_ASSERT (fatal halt) is used for programming errors such as
 *   pool exhaustion and duplicate signals.  These are not recoverable
 *   runtime conditions; the system should stop.
 *
 * - OS_TimerCreate infers the owning HSM via OS_HsmGetCurrent().
 *   Timers may only be created during event dispatch (from an HSM
 *   state handler).  This coupling is intentional.
 *
 * - OwnerState-based auto-deletion on transitions is intentional.
 *
 * - Watchdog lives in OS_Watchdog.h / OS_Watchdog.c, separate from
 *   the timer module.
 */
#ifndef OS_CONFIG_H
#define OS_CONFIG_H

/* ------------------------------------------------------------------ */
/*  Event queue                                                       */
/* ------------------------------------------------------------------ */

/**
 * @brief Maximum events in the queue (must be a power of two).
 *
 * Bumped from 32 to 512 to absorb bursts (e.g. several timers and
 * ISRs posting in the same tick) without ever hitting the overflow
 * Q_ASSERT. Sized at design time — no runtime occupancy metric is
 * tracked by the event module (intentional, see OS_Event.c).
 */
#define OS_MAX_EVENTS        512U

/** @brief Bit-mask for O(1) circular-buffer indexing. */
#define OS_EVENT_MASK        (OS_MAX_EVENTS - 1U)

/* ------------------------------------------------------------------ */
/*  Timing wheel                                                      */
/* ------------------------------------------------------------------ */

/**
 * @brief Timing-wheel width in bits.
 *
 * Pool size = wheel size = 2^OS_TIMER_WHEEL_BITS.
 * Increasing this adds more simultaneous timers and more RAM.
 * Default: 4  → 16 slots / 16 simultaneous timers.
 */
#define OS_TIMER_WHEEL_BITS  4U

/** @brief Number of timing-wheel slots (= maximum simultaneous timers). */
#define OS_TIMER_WHEEL_SIZE  (1U << OS_TIMER_WHEEL_BITS)

/** @brief Bitmask for O(1) wheel-slot indexing. */
#define OS_TIMER_WHEEL_MASK  (OS_TIMER_WHEEL_SIZE - 1U)

/* ------------------------------------------------------------------ */
/*  Per-HSM timer quota                                               */
/* ------------------------------------------------------------------ */

/**
 * @brief Maximum number of simultaneous timers per HSM instance.
 *
 * Prevents one HSM from exhausting the shared pool.  All HSMs share
 * the same quota.  Must be <= OS_TIMER_WHEEL_SIZE.
 */
#define OS_TIMER_MAX_PER_HSM  4U

/* ------------------------------------------------------------------ */
/*  Tick period                                                       */
/* ------------------------------------------------------------------ */

/**
 * @brief Duration of one OS tick in milliseconds.
 *
 * Set to match the hardware timer interval configured in the port.
 * All OS_TimerCreate() periods are expressed in ticks, not in ms.
 * Example: OS_TICK_PERIOD_MS = 1   →  1 tick = 1 ms
 *          OS_TICK_PERIOD_MS = 10  →  1 tick = 10 ms
 *          OS_TICK_PERIOD_MS = 1000 → 1 tick = 1 s  (enables 20+ year timers)
 *
 * @note Changing this value requires matching hardware timer
 *       reconfiguration in the port layer (Port_SysTickStart).
 */
#define OS_TICK_PERIOD_MS    1U

/* ------------------------------------------------------------------ */
/*  Watchdog                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief Minimum allowed watchdog timeout in ticks.
 *
 * OS_WatchdogInit() asserts that the requested timeout is at least
 * this value.  Prevents a timeout of 0 or 1 that would fire on the
 * very first tick.
 */
#define OS_WATCHDOG_MIN_TICKS  2U

/* ------------------------------------------------------------------ */
/*  HSM                                                               */
/* ------------------------------------------------------------------ */

/** @brief Maximum nesting depth of an HSM (top + children). */
#define OS_HSM_MAX_DEPTH     4U

#endif /* OS_CONFIG_H */
