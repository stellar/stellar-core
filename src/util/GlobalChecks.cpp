// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "GlobalChecks.h"

#ifdef _WIN32
#include <Windows.h>
#endif
#include <cassert>
#include <thread>

namespace stellar
{
static std::thread::id mainThread = std::this_thread::get_id();

void
assertThreadIsMain()
{
    dbgAssert(mainThread == std::this_thread::get_id());
}

void
dbgAbort()
{
#ifdef _WIN32
    DebugBreak();
#else
    abort();
#endif
}
}