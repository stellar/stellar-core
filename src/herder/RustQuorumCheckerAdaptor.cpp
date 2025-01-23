// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/RustQuorumCheckerAdaptor.h"
#include "crypto/KeyUtils.h"
#include "herder/HerderUtils.h"
#include "rust/RustBridge.h"
#include "util/Logging.h"
#include <chrono>

namespace stellar
{

std::atomic<uint32_t> RustQuorumCheckerAdaptor::mSuccessfulCallCount{0};
std::atomic<uint32_t> RustQuorumCheckerAdaptor::mFailedCallCount{0};
std::atomic<uint32_t> RustQuorumCheckerAdaptor::mInterruptedCallCount{0};
std::atomic<uint32_t> RustQuorumCheckerAdaptor::mPotentialSplitCount{0};

bool
RustQuorumCheckerAdaptor::networkEnjoysQuorumIntersection(
    QuorumTracker::QuorumMap const& qmap,
    std::optional<stellar::Config> const& cfg,
    rust_bridge::quorum_checker::Interrupt const& interrupt,
    QuorumIntersectionChecker::PotentialSplit& potentialSplit)
{
    return networkEnjoysQuorumIntersection(toQuorumIntersectionMap(qmap), cfg,
                                           interrupt, potentialSplit);
}

bool
RustQuorumCheckerAdaptor::networkEnjoysQuorumIntersection(
    QuorumIntersectionChecker::QuorumSetMap const& qmap,
    std::optional<stellar::Config> const& cfg,
    rust_bridge::quorum_checker::Interrupt const& interrupt,
    QuorumIntersectionChecker::PotentialSplit& potentialSplit)
{
    rust::Vec<CxxBuf> nodesBuf;
    rust::Vec<CxxBuf> quorumSetsBuf;
    nodesBuf.reserve(qmap.size());
    quorumSetsBuf.reserve(qmap.size());
    for (auto const& pair : qmap)
    {
        if (pair.second)
        {
            nodesBuf.push_back(toCxxBuf(pair.first));
            quorumSetsBuf.push_back(toCxxBuf(*pair.second));
        }
        else
        {
            CLOG_DEBUG(SCP, "Node with missing QSet: {}",
                       toShortString(cfg, pair.first));
        }
    }

    QuorumSplit split;
    QuorumCheckerStatus status;
    try
    {
        auto start = std::chrono::steady_clock::now();
        status =
            rust_bridge::quorum_checker::network_enjoys_quorum_intersection(
                nodesBuf, quorumSetsBuf, interrupt, split);
        auto end = std::chrono::steady_clock::now();
        auto duration =
            std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        CLOG_DEBUG(SCP, "Quorum intersection check took {} microseconds",
                   duration.count());
        ++mSuccessfulCallCount;
    }
    catch (const std::exception& e)
    {
        ++mFailedCallCount;
        throw RustQuorumCheckerError(e.what());
    }

    if (status == QuorumCheckerStatus::UNKNOWN)
    {
        ++mInterruptedCallCount;
        throw QuorumIntersectionChecker::InterruptedException();
    }
    else if (status == QuorumCheckerStatus::SAT)
    {
        ++mPotentialSplitCount;
        potentialSplit = toQuorumSplitNodeIDs(split);
    }
    return status == QuorumCheckerStatus::UNSAT;
}

} // namespace stellar {