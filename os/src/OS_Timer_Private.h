/**
 * @file  OS_Timer_Private.h
 * @brief OS-internal timer API — NOT part of the public interface.
 *
 * Include this header ONLY from OS source files (os/src/).
 * User application code must never include it directly.
 */
#ifndef OS_TIMER_PRIVATE_H
#define OS_TIMER_PRIVATE_H

/**
 * @brief Delete every timer owned by the current state.
 *
 * Called automatically by OS_Hsm during state transitions.
 * The owning HSM and state are resolved internally via
 * OS_HsmGetCurrent() and OS_HsmGetDispatchDepth().
 *
 * @note O(m) in the number of timers owned by that state, bounded by
 *       OS_TIMER_MAX_PER_HSM — effectively O(1) in practice.
 *       This is intentional: called only during transitions, never
 *       from the tick ISR.
 */
void OS_TimerDeleteByState(void);

#endif /* OS_TIMER_PRIVATE_H */
