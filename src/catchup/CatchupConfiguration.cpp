// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/CatchupConfiguration.h"
#include <cassert>

namespace stellar
{

CatchupConfiguration::CatchupConfiguration(uint32_t toLedger, uint32_t count)
    : mToLedger{toLedger}, mCount{count}
{
}

CatchupConfiguration
CatchupConfiguration::resolve(uint32_t remoteCheckpoint) const
{
    auto resolvedToLedger = (toLedger() == CatchupConfiguration::CURRENT)
                                ? remoteCheckpoint
                                : toLedger();
    return CatchupConfiguration{resolvedToLedger, count()};
}
}
