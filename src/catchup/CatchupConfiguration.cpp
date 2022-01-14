// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/CatchupConfiguration.h"

#include <cassert>
#include <fmt/format.h>

namespace stellar
{

void
CatchupConfiguration::checkInvariants() const
{
    if (mMode == CatchupConfiguration::Mode::LOCAL_BUCKETS_ONLY)
    {
        releaseAssert(mHAS && mHistoryEntry);
        releaseAssert(toLedger() != CatchupConfiguration::CURRENT);
        releaseAssert(count() == 0);
    }
    else
    {
        releaseAssert(!mHAS && !mHistoryEntry);
    }
}

CatchupConfiguration::CatchupConfiguration(LedgerNumHashPair ledgerHashPair,
                                           uint32_t count, Mode mode)
    : mCount{count}, mLedgerHashPair{ledgerHashPair}, mMode{mode}
{
    checkInvariants();
}

CatchupConfiguration::CatchupConfiguration(HistoryArchiveState has,
                                           LedgerHeaderHistoryEntry lhhe)
    : mCount(0)
    , mLedgerHashPair(LedgerNumHashPair(lhhe.header.ledgerSeq,
                                        std::make_optional(lhhe.hash)))
    , mMode(CatchupConfiguration::Mode::LOCAL_BUCKETS_ONLY)
    , mHAS(std::make_optional(has))
    , mHistoryEntry(std::make_optional(lhhe))
{
    checkInvariants();
}

CatchupConfiguration::CatchupConfiguration(uint32_t toLedger, uint32_t count,
                                           Mode mode)
    : mCount{count}, mLedgerHashPair{toLedger, std::nullopt}, mMode{mode}
{
    checkInvariants();
}

CatchupConfiguration
CatchupConfiguration::resolve(uint32_t remoteCheckpoint) const
{
    checkInvariants();
    auto cfg = *this;
    if (toLedger() == CatchupConfiguration::CURRENT)
    {
        cfg.mLedgerHashPair.first = remoteCheckpoint;
        return cfg;
    }
    return cfg;
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
            fmt::format(FMT_STRING("{} is not a valid ledger number"), str));
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
            fmt::format(FMT_STRING("{} is not a valid ledger count"), str));
    }

    return result;
}
}
