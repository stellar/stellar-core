#pragma once

#include <string>
#include <string_view>

#include "util/Logging.h"

namespace stellar
{

// Function to call when something has gone wrong. Logs an error (errorMessage,
// errorExtra...). In testing, also throw an exception. Otherwise, add a note to
// report the bug.
template <typename... Ts>
void
reportError(std::string_view exceptionMessage, std::string_view errorMessage,
            Ts... errorExtra)
{
    CLOG_ERROR(Overlay, errorMessage, errorExtra...);
#ifdef BUILD_TESTS
    throw std::runtime_error(std::string{exceptionMessage});
#else
    CLOG_ERROR(Overlay, "{}", REPORT_INTERNAL_BUG);
#endif
}
}
