/**
 * @file  OS_Error.h
 * @brief Assertion macros and fatal-error handler for the RTEF OS.
 */
#ifndef OS_ERROR_H
#define OS_ERROR_H

#include "OS_Types.h"

/**
 * @brief General assertion – halts and logs on failure.
 * @param test_ Boolean expression to evaluate.
 */
#define Q_ASSERT(test_) \
    ((test_) ? (void)0 \
             : OS_ErrorHandler(0U, #test_, __FILE__, (OS_U32)__LINE__))

/**
 * @brief Assertion with an explicit numeric error id.
 * @param id_   Numeric error identifier.
 * @param test_ Boolean expression to evaluate.
 */
#define Q_ASSERT_ID(id_, test_) \
    ((test_) ? (void)0 \
             : OS_ErrorHandler((OS_U32)(id_), #test_, __FILE__, \
                               (OS_U32)__LINE__))

/**
 * @brief Fatal error handler.
 *
 * Logs the error context (id, description, file, line and current OS
 * state) via the port layer and then halts the system.  Never returns.
 *
 * @param id   Numeric error identifier.
 * @param desc Stringified assertion / description.
 * @param file Source file name.
 * @param line Source line number.
 */
void OS_ErrorHandler(OS_U32 id, char const *desc,
                     char const *file, OS_U32 line);

#endif /* OS_ERROR_H */
