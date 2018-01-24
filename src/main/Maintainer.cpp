// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Maintainer.h"
#include "main/ExternalQueue.h"
#include "util/Logging.h"

namespace stellar
{

Maintainer::Maintainer(Application& app) : mApp{app}
{
}

void
Maintainer::performMaintenance(uint32_t count)
{
    LOG(INFO) << "Performing maintenance";
    ExternalQueue ps{mApp};
    ps.deleteOldEntries(count);
}
}
