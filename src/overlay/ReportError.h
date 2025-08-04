#pragma once

#include "util/Logging.h"

// Function-like macro to call when something has gone wrong. Logs an error
// (__VA_ARGS__). In testing, also throw an exception. Otherwise, add a
// note to report the bug.
#ifdef BUILD_TESTS
#define reportError(exceptionMessage, ...) \
    do \
    { \
        CLOG_ERROR(Overlay, __VA_ARGS__); \
        throw std::runtime_error(exceptionMessage); \
    } while (0)
#else
#define reportError(exceptionMessage, errorMessage, ...) \
    do \
    { \
        CLOG_ERROR(Overlay, __VA_ARGS__); \
        CLOG_ERROR(Overlay, "{}", REPORT_INTERNAL_BUG); \
    } while (0)
#endif
