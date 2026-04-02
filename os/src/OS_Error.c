/**
 * @file  OS_Error.c
 * @brief Fatal-error handler implementation.
 */
#include "OS_Error.h"
#include "OS_Hsm.h"
#include "OS_Port.h"

/* ------------------------------------------------------------------ */
void OS_ErrorHandler(OS_U32 id, char const *desc,
                     char const *file, OS_U32 line)
{
    Port_CriticalEnter();
    Port_ErrorLog(id, desc, file, line);
    Port_SystemHalt();

    /* Port_SystemHalt should never return; infinite loop as safeguard. */
    for (;;) { /* MISRA: empty loop body */ }
}
