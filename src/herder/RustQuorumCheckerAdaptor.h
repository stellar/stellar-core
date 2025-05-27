#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/QuorumTracker.h"
#include "main/Config.h"
#include "process/ProcessManager.h"
#include "rust/RustBridge.h"
#include <optional>

namespace Json
{
class Value;
}

namespace medida
{
class MetricsRegistry;
}

namespace stellar
{
struct QuorumMapIntersectionState;

namespace quorum_checker
{

struct QuorumCheckerMetrics
{
    uint64_t mSuccessfulCalls;
    uint64_t mFailedCalls;
    uint64_t mInterruptedCalls;
    uint64_t mPotentialSplits;
    uint64_t mCumulativeTimeMs;
    uint64_t mCumulativeMemBytes;
    QuorumCheckerMetrics();
    QuorumCheckerMetrics(Json::Value const& value);
    Json::Value toJson();
    void flush(medida::MetricsRegistry& metrics);
};

// In-process quorum intersection checker that directly calls the Rust
// implementation. Takes a JSON file containing the quorum map as input and
// writes results to an output JSON file. If analyzeCriticalGroups is true,
// performs additional analysis to identify critical node groups, with the time
// limit applying to the total analysis time across all runs. Resource limits
// (time and memory) are enforced by the Rust implementation.
QuorumCheckerStatus networkEnjoysQuorumIntersection(
    std::string const& jsonPath, uint64_t timeLimitMs, size_t memoryLimitBytes,
    bool analyzeCriticalGroups, std::string const& outJsonPath);

// Out-of-process quorum intersection checker that runs as a separate process.
// Serializes the quorum map to a JSON file, then spawns a new stellar-core
// process with the --check-quorum-intersection flag to perform the actual
// analysis. Results are read from the output JSON file and propagated back to
// the main process through the provided QuorumMapIntersectionState.
void runQuorumIntersectionCheckAsync(
    Hash const curr, uint32 ledger, std::string const& tmpDirName,
    QuorumTracker::QuorumMap const& qmap,
    std::weak_ptr<QuorumMapIntersectionState> hState, ProcessManager& pm,
    uint64_t timeLimitMs, size_t memoryLimitBytes, bool analyzeCriticalGroups);

} // namespace quorum_checker

class RustQuorumCheckerError : public std::runtime_error
{
  public:
    RustQuorumCheckerError(std::string const& msg) : std::runtime_error(msg)
    {
    }
};

} // namespace stellar
