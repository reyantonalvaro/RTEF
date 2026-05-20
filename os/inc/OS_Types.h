/**
 * @file  OS_Types.h
 * @brief Common types, signals and state-handler typedef for the RTEF OS.
 */
#ifndef OS_TYPES_H
#define OS_TYPES_H

#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/*  Basic fixed-width types                                           */
/* ------------------------------------------------------------------ */
typedef uint8_t   OS_U8;
typedef uint16_t  OS_U16;
typedef uint32_t  OS_U32;
typedef int8_t    OS_I8;
typedef int16_t   OS_I16;
typedef int32_t   OS_I32;

/* ------------------------------------------------------------------ */
/*  Signals                                                           */
/* ------------------------------------------------------------------ */

/** @brief Signal type carried by every event. */
typedef OS_U16 OS_Signal;

/** @brief Reserved system signals. User signals start at Q_USER. */
enum {
    Q_EMPTY = 0U,   /**< No signal / empty slot.          */
    Q_INIT  = 1U,   /**< Initialise children.             */
    Q_ENTRY = 2U,   /**< State entry action.              */
    Q_EXIT  = 3U,   /**< State exit action.               */
    Q_USER  = 4U    /**< First user-definable signal.     */
};

/* ------------------------------------------------------------------ */
/*  Event                                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief Lightweight event passed to every state handler.
 *
 * Param was added so events can carry a small payload (a value, a
 * handle, a packed status) without exposing globals to handlers.
 * System signals (Q_ENTRY/Q_EXIT/Q_INIT) always use Param == 0.
 */
typedef struct {
    OS_Signal Signal;
    OS_U32    Param;
} OS_Event;

/* ------------------------------------------------------------------ */
/*  State handler                                                     */
/* ------------------------------------------------------------------ */

/** @brief Forward declaration – full definition lives in OS_Hsm.h. */
typedef struct OS_Hsm OS_Hsm;

/** @brief Value returned by a state handler. */
typedef enum {
    OS_HANDLED   = 0,  /**< Event was consumed.                      */
    OS_UNHANDLED = 1   /**< Event not handled – propagate to parent. */
} OS_Status;

/**
 * @brief State handler function pointer.
 *
 * @param me  Pointer to the HSM instance (the "hook").
 * @param e   Pointer to the dispatched event.
 * @return    OS_HANDLED or OS_UNHANDLED.
 */
typedef OS_Status (*OS_StateHandler)(OS_Hsm *const me,
                                     OS_Event const *const e);

#endif /* OS_TYPES_H */
