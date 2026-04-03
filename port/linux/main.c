/**
 * @file  main.c
 * @brief Traffic-light demo for the RTEF OS on Linux.
 *
 * Demonstrates a single hierarchical state machine with three child
 * states (Red, Green, Yellow) cycling via software timers.
 *
 * @code
 *   Operating  (top state)
 *     ├── Red    ──(500 ms)──► Green
 *     ├── Green  ──(500 ms)──► Yellow
 *     └── Yellow ──(200 ms)──► Red
 * @endcode
 */
#define _POSIX_C_SOURCE 199309L

#include "OS_Config.h"
#include "OS_Types.h"
#include "OS_Port.h"
#include "OS_Error.h"
#include "OS_Event.h"
#include "OS_Timer.h"
#include "OS_Hsm.h"

#include <stdio.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/*  User signals                                                      */
/* ------------------------------------------------------------------ */

enum {
    SIG_TIMEOUT = Q_USER   /**< Timer expired.  */
};

/* ------------------------------------------------------------------ */
/*  HSM instance (the hook)                                           */
/* ------------------------------------------------------------------ */

static OS_Hsm TrafficLight;

/* ------------------------------------------------------------------ */
/*  Forward declarations of state handlers                            */
/* ------------------------------------------------------------------ */

static OS_Status StateOperating(OS_Hsm *const me, OS_Event const *const e);
static OS_Status StateRed      (OS_Hsm *const me, OS_Event const *const e);
static OS_Status StateGreen    (OS_Hsm *const me, OS_Event const *const e);
static OS_Status StateYellow   (OS_Hsm *const me, OS_Event const *const e);

/* ------------------------------------------------------------------ */
/*  State handlers                                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief Top-level "Operating" state.
 */
static OS_Status StateOperating(OS_Hsm *const me, OS_Event const *const e)
{
    OS_Status status = OS_HANDLED;

    switch (e->Signal) {
        case Q_INIT:
            OS_HsmChildInit(me, StateRed);
            break;
        case Q_ENTRY:
            (void)printf("[HSM] ENTRY  Operating\n");
            break;
        case Q_EXIT:
            (void)printf("[HSM] EXIT   Operating\n");
            break;
        default:
            status = OS_UNHANDLED;
            break;
    }
    return status;
}

/**
 * @brief Red-light state – 500 ms timeout then transition to Green.
 */
static OS_Status StateRed(OS_Hsm *const me, OS_Event const *const e)
{
    OS_Status status = OS_HANDLED;

    switch (e->Signal) {
        case Q_ENTRY:
            (void)printf("[HSM] ENTRY  Red    \xF0\x9F\x94\xB4\n");
            (void)OS_TimerCreate((OS_Signal)SIG_TIMEOUT, 500U, false);
            break;
        case Q_EXIT:
            (void)printf("[HSM] EXIT   Red\n");
            break;
        case (OS_Signal)SIG_TIMEOUT:
            (void)printf("[HSM] Red -> Green\n");
            OS_HsmTransition(me, StateGreen);
            break;
        default:
            status = OS_UNHANDLED;
            break;
    }
    return status;
}

/**
 * @brief Green-light state – 500 ms timeout then transition to Yellow.
 */
static OS_Status StateGreen(OS_Hsm *const me, OS_Event const *const e)
{
    OS_Status status = OS_HANDLED;

    switch (e->Signal) {
        case Q_ENTRY:
            (void)printf("[HSM] ENTRY  Green  \xF0\x9F\x9F\xA2\n");
            (void)OS_TimerCreate((OS_Signal)SIG_TIMEOUT, 500U, false);
            break;
        case Q_EXIT:
            (void)printf("[HSM] EXIT   Green\n");
            break;
        case (OS_Signal)SIG_TIMEOUT:
            (void)printf("[HSM] Green -> Yellow\n");
            OS_HsmTransition(me, StateYellow);
            break;
        default:
            status = OS_UNHANDLED;
            break;
    }
    return status;
}

/**
 * @brief Yellow-light state – 200 ms timeout then transition to Red.
 */
static OS_Status StateYellow(OS_Hsm *const me, OS_Event const *const e)
{
    OS_Status status = OS_HANDLED;

    switch (e->Signal) {
        case Q_ENTRY:
            (void)printf("[HSM] ENTRY  Yellow \xF0\x9F\x9F\xA1\n");
            (void)OS_TimerCreate((OS_Signal)SIG_TIMEOUT, 200U, false);
            break;
        case Q_EXIT:
            (void)printf("[HSM] EXIT   Yellow\n");
            break;
        case (OS_Signal)SIG_TIMEOUT:
            (void)printf("[HSM] Yellow -> Red\n");
            OS_HsmTransition(me, StateRed);
            break;
        default:
            status = OS_UNHANDLED;
            break;
    }
    return status;
}

/* ------------------------------------------------------------------ */
/*  Main                                                              */
/* ------------------------------------------------------------------ */

/** @brief Demo duration in milliseconds. */
#define DEMO_DURATION_MS  3500U

int main(void)
{
    struct timespec idle;
    idle.tv_sec  = 0;
    idle.tv_nsec = 500000L;   /* 0.5 ms idle sleep */

    (void)printf("=== RTEF OS – Traffic Light Demo ===\n\n");

    /* 1. Platform init */
    Port_Init();

    /* 2. OS sub-system init */
    OS_EventInit();
    OS_TimerInit();

    /* 3. Create the traffic-light HSM */
    OS_HsmInit(&TrafficLight, StateOperating);

    /* 4. Start the 1 ms tick */
    Port_SysTickStart();

    /* 5. Main super-loop */
    while (OS_GetTickCount() < (OS_U32)DEMO_DURATION_MS) {
        OS_EventDispatch();
        (void)nanosleep(&idle, (struct timespec *)0);
    }

    /* 6. Shut down */
    Port_SysTickStop();
    (void)printf("\n=== Demo finished (%u ms) ===\n",
                 (unsigned)OS_GetTickCount());

    return 0;
}
