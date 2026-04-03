/**
 * @file  OS_Hsm.h
 * @brief Hierarchical State Machine (HSM) engine for the RTEF OS.
 */
#ifndef OS_HSM_H
#define OS_HSM_H

#include "OS_Types.h"
#include "OS_Config.h"

/* ------------------------------------------------------------------ */
/*  HSM instance (the "hook")                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief HSM instance structure.
 *
 * Allocate statically.  The address of the variable is the unique
 * "hook" that identifies the HSM throughout the OS.
 */
struct OS_Hsm {
    OS_StateHandler State[OS_HSM_MAX_DEPTH]; /**< Active handler per depth. */
    OS_I16          TimerHead;                /**< Per-HSM active-timer list.*/
    OS_U8           Depth;                    /**< Deepest active level.     */
    OS_U8           TimerCount;               /**< Active timers owned.      */
    bool            Initialized;              /**< Double-init guard.        */
};

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief Register and start a top-level HSM.
 *
 * Dispatches Q_ENTRY and Q_INIT to @p topState.  Fires Q_ASSERT if
 * the HSM was already initialised.
 *
 * @param me        HSM instance (hook).
 * @param topState  Root state handler.
 */
void OS_HsmInit(OS_Hsm *me, OS_StateHandler topState);

/**
 * @brief Activate a child state during Q_INIT of the parent.
 *
 * Must only be called from inside a Q_INIT handler.  Dispatches
 * Q_ENTRY and Q_INIT to @p childState recursively.
 *
 * @param me          HSM instance (hook).
 * @param childState  Child state handler to activate.
 */
void OS_HsmChildInit(OS_Hsm *me, OS_StateHandler childState);

/**
 * @brief Transition to a sibling state at the same depth.
 *
 * Exits every descendant (bottom-up), exits the current state,
 * enters the target and dispatches Q_INIT.  Only callable during
 * event dispatch.
 *
 * @param me      HSM instance (hook).
 * @param target  Target sibling state handler.
 */
void OS_HsmTransition(OS_Hsm *me, OS_StateHandler target);

/* ------------------------------------------------------------------ */
/*  OS-internal helpers                                                */
/* ------------------------------------------------------------------ */

/**
 * @brief Dispatch a signal to an HSM (called by OS_EventDispatch).
 * @param me      HSM instance.
 * @param signal  Signal to dispatch.
 */
void OS_HsmDispatch(OS_Hsm *me, OS_Signal signal);

/**
 * @brief Get the HSM currently being dispatched (NULL when idle).
 */
OS_Hsm *OS_HsmGetCurrent(void);

/**
 * @brief Check whether the OS is inside a dispatch.
 */
bool OS_HsmInDispatch(void);

/**
 * @brief Current dispatch depth (valid only when OS_HsmInDispatch()).
 */
OS_U8 OS_HsmGetDispatchDepth(void);

#endif /* OS_HSM_H */
