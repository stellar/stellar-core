// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Thread.h"
#include "util/Logging.h"

#ifdef _WIN32
#else
#include <unistd.h>
#endif

namespace stellar
{

#if defined(_WIN32)

void
runCurrentThreadWithLowPriority()
{
    auto native = std::this_thread::get_id();
    auto ret = SetThreadPriority(native, THREAD_PRIORITY_BELOW_NORMAL);

    if (!ret)
    {
        CLOG(DEBUG, "Fs") << "Unable to set priority for thread: " << ret;
    }
}

#elif defined(__linux__)

void
runCurrentThreadWithLowPriority()
{
    constexpr auto const LOW_PRIORITY_NICE = 5;

    auto newNice = nice(LOW_PRIORITY_NICE);
    if (newNice != LOW_PRIORITY_NICE)
    {
        CLOG(DEBUG, "Fs") << "Unable to run worker thread with low priority. "
                             "Normal priority will be used.";
    }
}

#elif defined(__APPLE__)

void
runCurrentThreadWithLowPriority()
{
    // probably can use thread_policy_set with THREAD_PRECEDENCE_POLICY to get
    // desired effect
}

#else

void
runCurrentThreadWithLowPriority()
{
}

#endif
}
