// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <string>

namespace stellar
{

// Logs an error. In testing, also throw an exception. Otherwise, add a
// note to report the bug.
void logErrorOrThrow(std::string const& message);
}
