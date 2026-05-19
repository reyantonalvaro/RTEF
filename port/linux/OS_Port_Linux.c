/**
 * @file  OS_Port_Linux.c
 * @brief RTEF OS port for Linux (POSIX threads).
 *
 * Implements the OS_Port.h interface using pthreads for the critical
 * section and a dedicated thread for the 1 ms SysTick.
 */
#define _POSIX_C_SOURCE 200112L
#define _XOPEN_SOURCE   600

#include "OS_Port.h"
#include "OS_Timer.h"
#include "OS_Error.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>

/* ------------------------------------------------------------------ */
/*  Module-private state                                              */
/* ------------------------------------------------------------------ */

static pthread_mutex_t CritMutex;
static pthread_t       TickThread;
static volatile bool   TickRunning  = false;

/* ------------------------------------------------------------------ */
/*  SysTick thread                                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief Background thread that calls OS_SysTick() every 1 ms.
 */
static void *SysTickTask(void *arg)
{
    struct timespec ts;
    ts.tv_sec  = 0;
    ts.tv_nsec = 1000000L;   /* 1 ms */

    (void)arg;

    while (TickRunning) {
        (void)nanosleep(&ts, (struct timespec *)0);
        OS_SysTick();
    }
    return (void *)0;
}

/* ------------------------------------------------------------------ */
/*  Port interface implementation                                     */
/* ------------------------------------------------------------------ */

void Port_Init(void)
{
    pthread_mutexattr_t attr;
    (void)pthread_mutexattr_init(&attr);
    (void)pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    (void)pthread_mutex_init(&CritMutex, &attr);
    (void)pthread_mutexattr_destroy(&attr);
}

void Port_CriticalEnter(void)
{
    (void)pthread_mutex_lock(&CritMutex);
}

void Port_CriticalExit(void)
{
    (void)pthread_mutex_unlock(&CritMutex);
}

void Port_ErrorLog(OS_U32 id, char const *desc,
                   char const *file, OS_U32 line)
{
    (void)fprintf(stderr,
                  "\n[OS ERROR] id=%u  desc=\"%s\"  file=%s  line=%u\n",
                  (unsigned)id, desc, file, (unsigned)line);
}

void Port_SystemHalt(void)
{
    (void)fflush(stderr);
    (void)fflush(stdout);
    abort();
}

void Port_SysTickStart(void)
{
    TickRunning = true;
    (void)pthread_create(&TickThread, (const pthread_attr_t *)0,
                         SysTickTask, (void *)0);
}

void Port_SysTickStop(void)
{
    TickRunning = false;
    (void)pthread_join(TickThread, (void **)0);
}

void Port_WatchdogInit(OS_U32 timeoutTicks)
{
    /* No hardware watchdog on Linux – handled entirely in software. */
    (void)timeoutTicks;
}

void Port_WatchdogFeed(void)
{
    /* Software-only – nothing to do on the HW side. */
}
