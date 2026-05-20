/**
 * @file  main.c
 * @brief Traffic-light demo for the RTEF OS on Linux.
 *
 *   Operating  (top state)
 *     +-- Red    --(500 ms)--> Green
 *     +-- Green  --(500 ms)--> Yellow
 *     +-- Yellow --(200 ms)--> Red
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

enum { SIG_TIMEOUT = Q_USER };

#define DEMO_DURATION_MS  3500U

static OS_Hsm TrafficLight;

/* ------------------------------------------------------------------ */
/*  States                                                             */
/* ------------------------------------------------------------------ */

static OS_Status StateOperating(OS_Hsm *const me, OS_Event const *const e);
static OS_Status StateRed      (OS_Hsm *const me, OS_Event const *const e);
static OS_Status StateGreen    (OS_Hsm *const me, OS_Event const *const e);
static OS_Status StateYellow   (OS_Hsm *const me, OS_Event const *const e);

/* Per-light parameters consumed by the shared LightHandler. */
typedef struct {
    char const     *label;
    char const     *icon;
    OS_U32          holdTicks;
    OS_StateHandler next;
} LightSpec;

static LightSpec const LightRed    = { "Red   ", "\xF0\x9F\x94\xB4", 500U, StateGreen  };
static LightSpec const LightGreen  = { "Green ", "\xF0\x9F\x9F\xA2", 500U, StateYellow };
static LightSpec const LightYellow = { "Yellow", "\xF0\x9F\x9F\xA1", 200U, StateRed    };

static OS_Status LightHandler(OS_Hsm *const me, OS_Event const *const e,
                              LightSpec const *const spec)
{
    OS_Status status = OS_HANDLED;

    switch (e->Signal) {
        case Q_ENTRY:
            (void)printf("[HSM] ENTRY  %s %s\n", spec->label, spec->icon);
            (void)OS_TimerCreate((OS_Signal)SIG_TIMEOUT, spec->holdTicks, false);
            break;
        case Q_EXIT:
            (void)printf("[HSM] EXIT   %s\n", spec->label);
            break;
        case (OS_Signal)SIG_TIMEOUT:
            (void)printf("[HSM] %s -> next\n", spec->label);
            OS_HsmTransition(me, spec->next);
            break;
        default:
            status = OS_UNHANDLED;
            break;
    }
    return status;
}

static OS_Status StateOperating(OS_Hsm *const me, OS_Event const *const e)
{
    OS_Status status = OS_HANDLED;

    switch (e->Signal) {
        case Q_INIT:  OS_HsmChildInit(me, StateRed);                  break;
        case Q_ENTRY: (void)printf("[HSM] ENTRY  Operating\n");       break;
        case Q_EXIT:  (void)printf("[HSM] EXIT   Operating\n");       break;
        default:      status = OS_UNHANDLED;                          break;
    }
    return status;
}

static OS_Status StateRed   (OS_Hsm *const me, OS_Event const *const e) { return LightHandler(me, e, &LightRed);    }
static OS_Status StateGreen (OS_Hsm *const me, OS_Event const *const e) { return LightHandler(me, e, &LightGreen);  }
static OS_Status StateYellow(OS_Hsm *const me, OS_Event const *const e) { return LightHandler(me, e, &LightYellow); }

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    struct timespec const idle = { 0, 500000L };   /* 0.5 ms idle */

    (void)printf("=== RTEF OS - Traffic Light Demo ===\n\n");

    Port_Init();
    OS_TimerInit();
    OS_HsmInit(&TrafficLight, StateOperating);
    Port_SysTickStart();

    while (OS_GetTickCount() < (OS_U32)DEMO_DURATION_MS) {
        /* Drain all pending events, then sleep until the next tick or
         * ISR. OS_EventDispatch returns false when the queue is empty,
         * which is what enables the idle wait below (no busy-poll). */
        while (OS_EventDispatch()) {
        }
        (void)nanosleep(&idle, (struct timespec *)0);
    }

    Port_SysTickStop();
    (void)printf("\n=== Demo finished (%u ms) ===\n",
                 (unsigned)OS_GetTickCount());
    return 0;
}
