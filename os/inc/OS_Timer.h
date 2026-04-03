/**
 * @file  OS_Timer.h
 * @brief Software-timer management for the RTEF OS.
 *
 * @par Time units
 * All durations are expressed in **ticks**, not milliseconds.
 * One tick corresponds to OS_TICK_PERIOD_MS milliseconds (configured in
 * OS_Config.h).  Example: to get a 500 ms timeout with the default
 * OS_TICK_PERIOD_MS = 1, use periodTicks = 500.
 *
 * @par Design decisions
 * - Timing wheel with per-timer Round counter: O(1) insert/remove/tick
 *   for arbitrarily long periods (up to 2^32 ticks).
 * - Pool exhaustion and duplicate signals trigger Q_ASSERT (intentional
 *   fail-fast).
 * - OS_TimerCreate only works during HSM event dispatch (intentional).
 * - OwnerState-based auto-deletion is intentional.
 * - Per-HSM timer quota (OS_TIMER_MAX_PER_HSM) prevents pool starvation.
 */
#ifndef OS_TIMER_H
#define OS_TIMER_H

#include "OS_Types.h"

/* ------------------------------------------------------------------ */
/*  Timer handle                                                      */
/* ------------------------------------------------------------------ */

/** @brief Opaque handle returned by OS_TimerCreate. */
typedef struct {
    OS_U16 Index;       /**< Pool slot index.          */
    OS_U16 Generation;  /**< Stale-handle guard value. */
} OS_TimerHandle;

/**
 * @brief Sentinel value for an invalid / unused handle.
 *
 * Active timers always have Generation >= 1, so Generation = 0 is
 * never a valid active-timer generation.
 */
#define OS_TIMER_INVALID  ((OS_TimerHandle){ 0xFFFFU, 0U })

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief Initialise the timer sub-system.  Call once at start-up.
 */
void OS_TimerInit(void);

/**
 * @brief Create a new software timer.
 *
 * The owning HSM is obtained internally via OS_HsmGetCurrent().
 * This function **must** be called during HSM event dispatch (e.g.
 * from a Q_ENTRY handler).
 *
 * When the timer expires it posts @p signal to the event queue of
 * the owning HSM.  Attempting to create a timer with the same signal
 * as one already active in this HSM triggers Q_ASSERT.  Exceeding
 * OS_TIMER_MAX_PER_HSM timers per HSM or exhausting the pool also
 * triggers Q_ASSERT.
 *
 * @param signal        Signal to post on expiry.
 * @param periodTicks   Timeout / period in OS ticks (> 0).
 *                      See OS_TICK_PERIOD_MS for the tick duration.
 * @param periodic      true = auto-reload, false = one-shot.
 * @return              Handle for later manual deletion.
 */
OS_TimerHandle OS_TimerCreate(OS_Signal signal,
                              OS_U32 periodTicks, bool periodic);

/**
 * @brief Manually delete a timer by handle.
 *
 * Only the state that created the timer may delete it.
 * If the handle is stale (timer already expired or was never valid),
 * the function returns false without asserting.
 *
 * @param handle  Handle previously returned by OS_TimerCreate.
 * @return        true  = timer was found and deleted.
 *                false = handle is stale; timer is already gone.
 */
bool OS_TimerDelete(OS_TimerHandle handle);

/**
 * @brief 1-tick system-tick handler.  Call from the HW timer ISR.
 *
 * Inserts expired-timer events into the queue only; does not dispatch.
 * This is the highest-priority ISR context.  Event dispatch happens
 * in OS_EventDispatch() at lower priority.
 */
void OS_SysTick(void);

/**
 * @brief Return the current tick count (ticks since OS_TimerInit).
 * @return Tick count (atomic read, safe on 8/16/32-bit platforms).
 */
OS_U32 OS_GetTickCount(void);

#endif /* OS_TIMER_H */
