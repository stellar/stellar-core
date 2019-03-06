#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <stdint.h>
#include <utility>
#include <vector>

namespace stellar
{

class CatchupConfiguration;

extern std::vector<std::pair<uint32_t, CatchupConfiguration>>
    gCatchupRangeCases;
}
