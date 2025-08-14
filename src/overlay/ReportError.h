#pragma once

#include <string>

// Logs an error. In testing, also throw an exception. Otherwise, add a
// note to report the bug.
void reportError(std::string const& message);
