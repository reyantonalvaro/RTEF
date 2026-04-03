/**
 * @file  OS_Watchdog.c
 * @brief Software watchdog implementation for the RTEF OS.
 */
#include "OS_Watchdog.h"
#include "OS_Watchdog_Private.h"
#include "OS_Error.h"
#include "OS_Config.h"
#include "OS_Port.h"

/* ------------------------------------------------------------------ */
/*  Module-private state                                              */
/* ------------------------------------------------------------------ */

static OS_U32 WdgTimeout;
static OS_U32 WdgCounter;
static bool   WdgEnabled;

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

void OS_WatchdogInit(OS_U32 timeoutTicks)
{
    Q_ASSERT(timeoutTicks >= (OS_U32)OS_WATCHDOG_MIN_TICKS);

    Port_CriticalEnter();
    WdgTimeout = timeoutTicks;
    WdgCounter = timeoutTicks;
    WdgEnabled = true;
    Port_CriticalExit();

    Port_WatchdogInit(timeoutTicks);
}

/* ------------------------------------------------------------------ */
void OS_WatchdogFeed(void)
{
    Port_CriticalEnter();
    WdgCounter = WdgTimeout;
    Port_CriticalExit();

    Port_WatchdogFeed();
}

/* ------------------------------------------------------------------ */
/*  OS-internal                                                       */
/* ------------------------------------------------------------------ */

void OS_WatchdogTick(void)
{
    if (!WdgEnabled) {
        return;
    }

    if (WdgCounter > 1U) {
        WdgCounter--;
    } else {
        Q_ASSERT_ID(99U, false);   /* Software watchdog expired */
    }
}
