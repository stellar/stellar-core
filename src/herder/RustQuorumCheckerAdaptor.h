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
    uint64_t mSuccessfulRun;
    uint64_t mFailedRun;
    uint64_t mAbortedRun;
    uint64_t mResultPotentialSplit;
    uint64_t mResultUnknown;
    uint64_t mCumulativeTimeMs;
    uint64_t mCumulativeMemByte;
    QuorumCheckerMetrics();
    QuorumCheckerMetrics(Json::Value const& value);
    Json::Value toJson();
    void flush(medida::MetricsRegistry& metrics);
};

// In-process quorum intersection checker that directly calls the Rust
// implementation. Takes a JSON file containing the quorum map as input and
// writes results to an output JSON file. If analyzeCriticalGroups is true,
// performs additional analysis to identify critical node groups.
//
// Resources limits:
// - The time limit applies across all runs (all loops during criticality
//   analysis), and is enforced by the Rust implementation, which returns an
//   `Err` if exceeded.
// - The memory limit is enforced by the glocal allocator, and once exceeds,
//   will abort the program immediately.
//
// Therefore it is **crucial** this routine runs in a separate process from the
// main stellar-core!!
//
// Return values:
// - `UNSAT` if the quorum intersection check finds no non-intersecting quorums
//   (good)
// - `SAT` if the quorum intersection check finds quorum splits (bad!!)
// - `UNKNOWN` if the quorum intersection check does not complete, likely due to
//   exceeding solver internal limits (e.g. no. conflicts). Note: if the quorum
//   intersection check completes, but the criticality analysis
//   (analyzeCriticalGroups == true) is interrupted, the status of the
//   proceeding the quorum intersection check will be returned.
//
// The result JSON file contains additional information including the error
// message (if any), resource usage metrics, and other analysis results.
QuorumCheckerStatus networkEnjoysQuorumIntersection(
    std::string const& jsonPath, uint64_t timeLimitMs, size_t memoryLimitBytes,
    bool analyzeCriticalGroups, std::string const& outJsonPath);

// Out-of-process quorum intersection checker that runs as a separate process.
// Serializes the quorum map to a JSON file, then spawns a new stellar-core
// process with the --check-quorum-intersection flag to perform the actual
// analysis. Results are read from the output JSON file and propagated back to
// the main process through the provided QuorumMapIntersectionState.
void runQuorumIntersectionCheckAsync(
    Application& app, Hash const curr, uint32 ledger,
    std::string const& tmpDirName, QuorumTracker::QuorumMap const& qmap,
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
