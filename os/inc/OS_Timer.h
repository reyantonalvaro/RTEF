/**
 * @file  OS_Timer.h
 * @brief Software-timer management and watchdog for the RTEF OS.
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

/** @brief Sentinel for an invalid / unused handle. */
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
 * The owning HSM is obtained internally via OS_HsmGetCurrent(),
 * so this function must be called during event dispatch (e.g. from
 * a Q_ENTRY handler).  When the timer expires it posts @p signal
 * to the event queue of that HSM.  Duplicate timers (same hook +
 * same signal already active) trigger Q_ASSERT.
 *
 * @param signal    Signal to post on expiry.
 * @param periodMs  Timeout / period in milliseconds (> 0).
 * @param periodic  true = auto-reload, false = one-shot.
 * @return          Handle for later manual deletion.
 */
OS_TimerHandle OS_TimerCreate(OS_Signal signal,
                              OS_U32 periodMs, bool periodic);

/**
 * @brief Manually delete a timer by handle.
 *
 * Protection: only the state that created the timer may delete it.
 *
 * @param handle  Handle previously returned by OS_TimerCreate.
 */
void OS_TimerDelete(OS_TimerHandle handle);

/**
 * @brief Delete every timer owned by a specific state (OS-internal).
 *
 * Called automatically by the HSM engine during state transitions.
 *
 * @param hook   HSM instance.
 * @param state  State handler whose timers are to be removed.
 */
void OS_TimerDeleteByState(OS_Hsm *hook, OS_StateHandler state);

/**
 * @brief 1 ms system-tick handler.  Call from the HW timer ISR.
 */
void OS_SysTick(void);

/**
 * @brief Initialise the OS software watchdog.
 * @param timeoutMs  Watchdog timeout in milliseconds.
 */
void OS_WatchdogInit(OS_U32 timeoutMs);

/**
 * @brief Feed the software watchdog, resetting its counter.
 */
void OS_WatchdogFeed(void);

/**
 * @brief Return the current tick count (ms since OS_TimerInit).
 * @return Tick count.
 */
OS_U32 OS_GetTickCount(void);

#endif /* OS_TIMER_H */
