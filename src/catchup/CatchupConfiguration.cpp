// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/CatchupConfiguration.h"

#include <cassert>
#include <lib/util/format.h>

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

uint32_t
parseLedger(std::string const& str)
{
    if (str == "current")
    {
        return CatchupConfiguration::CURRENT;
    }

    auto pos = std::size_t{0};
    auto result = std::stoul(str, &pos);
    if (pos < str.length() || result < 2)
    {
        throw std::runtime_error(
            fmt::format("{} is not a valid ledger number", str));
    }

    return result;
}

uint32_t
parseLedgerCount(std::string const& str)
{
    if (str == "max")
    {
        return std::numeric_limits<uint32_t>::max();
    }

    auto pos = std::size_t{0};
    auto result = std::stoul(str, &pos);
    if (pos < str.length())
    {
        throw std::runtime_error(
            fmt::format("{} is not a valid ledger count", str));
    }

    return result;
}
}
