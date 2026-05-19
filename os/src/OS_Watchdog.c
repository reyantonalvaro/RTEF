/**
 * @file  OS_Watchdog.c
 * @brief Software watchdog implementation for the RTEF OS.
 *
 * Counter is advanced once per OS_SysTick() and reloaded by
 * OS_WatchdogFeed().  Reaching zero triggers a fatal Q_ASSERT.
 */
#include "OS_Watchdog.h"
#include "OS_Watchdog_Private.h"
#include "OS_Error.h"
#include "OS_Config.h"
#include "OS_Port.h"

static OS_U32 WdgTimeout;
static OS_U32 WdgCounter;
static bool   WdgEnabled;

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

void OS_WatchdogFeed(void)
{
    Port_CriticalEnter();
    WdgCounter = WdgTimeout;
    Port_CriticalExit();

    Port_WatchdogFeed();
}

/* OS-internal: called from OS_SysTick (already inside critical section). */
void OS_WatchdogTick(void)
{
    if (!WdgEnabled) {
        return;
    }
    WdgCounter--;
    if (WdgCounter == 0U) {
        Q_ASSERT_ID(99U, false);   /* Software watchdog expired. */
    }
}
