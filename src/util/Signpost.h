#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// This is a thin wrapper over Apple's os_signpost interface for noting
// point-of-interest intervals in a performance profile.

#if defined(__APPLE__)
#include <os/log.h>
#include <os/signpost.h>

namespace stellar
{
extern os_log_t signpostLogHandle;
void ensureSignpostLogHandle();
}

#define STELLAR_SIGNPOST_INTERVAL_BEGIN(name)                           \
do {                                                                    \
    stellar::ensureSignpostLogHandle();                                 \
    os_signpost_interval_begin(stellar::signpostLogHandle,              \
                               OS_SIGNPOST_ID_EXCLUSIVE, name);         \
 } while(0)

#define STELLAR_SIGNPOST_INTERVAL_END(name)                             \
    os_signpost_interval_end(stellar::signpostLogHandle,                \
                             OS_SIGNPOST_ID_EXCLUSIVE, name)            \

#else

#define STELLAR_SIGNPOST_INTERVAL_BEGIN(name) ((void)0)
#define STELLAR_SIGNPOST_INTERVAL_END(name)  ((void)0)

#endif
