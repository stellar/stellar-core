// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Thread.h"
#include "util/Logging.h"

#ifdef _WIN32
#else
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <pthread.h>
#endif

namespace stellar
{

#if defined(_WIN32)

void
runCurrentThreadWithLowPriority()
{
    HANDLE curThread = ::GetCurrentThread();
    BOOL ret = ::SetThreadPriority(curThread, THREAD_PRIORITY_BELOW_NORMAL);

    if (!ret)
    {
        LOG_DEBUG(DEFAULT_LOG, "Unable to set priority for thread: {}", ret);
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
        LOG_DEBUG(DEFAULT_LOG, "Unable to run worker thread with low priority. "
                               "Normal priority will be used.");
    }
}

#elif defined(__APPLE__)

void
runCurrentThreadWithLowPriority()
{
    // Default MacOS priority is 31 in a user-mode band from 0..63, niceing (or
    // other priority-adjustment) usually subtracts from there. Range is +/- 16,
    // with lower meaning lower (i.e. UTILITY class is 20). The standard
    // pthreads API works for adjusting a single thread's priority.
    constexpr auto const LOW_PRIORITY_NICE = 5;
    struct sched_param sp;
    int policy;
    int ret = pthread_getschedparam(pthread_self(), &policy, &sp);
    if (ret != 0)
    {
        LOG_DEBUG(DEFAULT_LOG, "Unable to get priority for thread: {}", ret);
    }
    sp.sched_priority -= LOW_PRIORITY_NICE;
    ret = pthread_setschedparam(pthread_self(), policy, &sp);
    if (ret != 0)
    {
        LOG_DEBUG(DEFAULT_LOG, "Unable to set priority for thread: {}", ret);
    }
}

#else

void
runCurrentThreadWithLowPriority()
{
}

#endif
}
