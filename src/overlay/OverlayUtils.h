#pragma once

#include <string>

namespace stellar
{

// Logs an error. In testing, also throw an exception. Otherwise, add a
// note to report the bug.
void logErrorOrThrow(std::string const& message);
}
