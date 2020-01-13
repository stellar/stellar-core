// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Signpost.h"

#if defined(__APPLE__)

namespace stellar
{
os_log_t signpostLogHandle = nullptr;

void
ensureSignpostLogHandle()
{
    if (!signpostLogHandle)
    {
        signpostLogHandle = os_log_create("org.stellar.core",
                                          OS_LOG_CATEGORY_POINTS_OF_INTEREST);
    }
}
}

#endif
