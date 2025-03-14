#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/QuorumIntersectionChecker.h"
#include "herder/QuorumTracker.h"
#include "main/Config.h"
#include "process/ProcessManager.h"
#include "rust/RustBridge.h"
#include <optional>
namespace stellar
{

// static adaptor class over rust QuorumChecker that exposes its functionalities
class RustQuorumCheckerAdaptor
{
    struct QuorumCheckerStats
    {
        uint32_t successfulCallCount{0};
        uint32_t failedCallCount{0};
        uint32_t interruptedCallCount{0};
        uint32_t potentialSplitCount{0};
        uint64_t cumulativeTimeMs{0};
        uint64_t cumulativeMemBytes{0};
    };

    static QuorumCheckerStats mStats;

    static QuorumCheckerStatus checkQuorumIntersectionInner(
        QuorumIntersectionChecker::QuorumSetMap const& qmap, QuorumSplit& split,
        QuorumCheckerResource const& limits, QuorumCheckerResource& usage);

  public:
    static QuorumCheckerStatus networkEnjoysQuorumIntersection(
        std::string const& jsonPath, uint64_t timeLimitMs,
        size_t memoryLimitBytes, bool analyzeCriticalGroups,
        std::string const& outJsonPath);

    static void runQuorumIntersectionCheckAsync(
        Hash const curr, uint32 ledger, std::string const& tmpDirName,
        QuorumTracker::QuorumMap const& qmap,
        std::weak_ptr<QuorumMapIntersectionState> hState, ProcessManager& pm,
        uint64_t timeLimitMs, size_t memoryLimitBytes,
        bool analyzeCriticalGroups);

}; // class RustQuorumCheckerAdaptor {

class RustQuorumCheckerError : public std::runtime_error
{
  public:
    RustQuorumCheckerError(std::string const& msg) : std::runtime_error(msg)
    {
    }
};

} // namespace stellar {
