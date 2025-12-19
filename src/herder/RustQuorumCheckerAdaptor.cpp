// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/RustQuorumCheckerAdaptor.h"
#include "crypto/KeyUtils.h"
#include "herder/HerderImpl.h"
#include "herder/HerderUtils.h"
#include "herder/QuorumIntersectionChecker.h"
#include "medida/meter.h"
#include "process/ProcessManager.h"
#include "rust/RustBridge.h"
#include "scp/LocalNode.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include "util/TmpDir.h"

namespace
{
// local helper functions to convert various types to and from json. their
// conventions need to be consistent (e.g. nodes are represented as full strkey)
Json::Value
toQuorumMapJson(QuorumTracker::QuorumMap const& qmap)
{
    Json::Value ret;
    ret["nodes"] = Json::Value(Json::arrayValue);

    for (auto const& elem : qmap)
    {
        Json::Value node;
        node["node"] = KeyUtils::toStrKey(elem.first);
        if (elem.second.mQuorumSet != nullptr)
        {
            node["qset"] = LocalNode::toJson(
                *elem.second.mQuorumSet,
                [&](PublicKey const& k) { return KeyUtils::toStrKey(k); });
        }
        else
        {
            node["qset"] = Json::Value(Json::objectValue);
        }
        ret["nodes"].append(node);
    }
    return ret;
}

Json::Value
toQuorumSplitJson(QuorumSplit const& split)
{
    Json::Value splitValue(Json::arrayValue);
    Json::Value left(Json::arrayValue);
    Json::Value right(Json::arrayValue);

    for (auto const& k : split.left)
    {
        left.append(std::string(k));
    }
    for (auto const& k : split.right)
    {
        right.append(std::string(k));
    }
    splitValue.append(left);
    splitValue.append(right);
    return splitValue;
}

void
fromQuorumSplitJson(QuorumIntersectionChecker::PotentialSplit& split,
                    Json::Value const& value)
{
    if (!value.isArray() || value.size() != 2 || !value[0].isArray() ||
        !value[1].isArray())
    {
        throw RustQuorumCheckerError(
            "Split JSON must be an array of two arrays");
    }

    split.first.clear();
    split.second.clear();
    split.first.reserve(value[0].size());
    split.second.reserve(value[1].size());

    // Parse left side
    for (auto const& nodeStr : value[0])
    {
        if (!nodeStr.isString())
        {
            throw RustQuorumCheckerError("Each node ID must be a string");
        }
        split.first.emplace_back(
            KeyUtils::fromStrKey<NodeID>(nodeStr.asString()));
    }

    // Parse right side
    for (auto const& nodeStr : value[1])
    {
        if (!nodeStr.isString())
        {
            throw RustQuorumCheckerError("Each node ID must be a string");
        }
        split.second.emplace_back(
            KeyUtils::fromStrKey<NodeID>(nodeStr.asString()));
    }
}

Json::Value
toCriticalGroupsJson(std::set<std::set<NodeID>> const& criticalGroups)
{
    Json::Value criticalValue(Json::arrayValue);
    for (auto const& group : criticalGroups)
    {
        Json::Value groupValue(Json::arrayValue);
        for (auto const& nd : group)
        {
            groupValue.append(KeyUtils::toStrKey(nd));
        }
        criticalValue.append(groupValue);
    }
    return criticalValue;
}

void
fromCriticalGroupsJson(std::set<std::set<NodeID>>& criticalGroups,
                       Json::Value const& value)
{
    if (!value.isArray())
    {
        throw RustQuorumCheckerError("Critical groups JSON must be an array");
    }

    criticalGroups.clear();
    for (auto const& groupValue : value)
    {
        if (!groupValue.isArray())
        {
            throw RustQuorumCheckerError(
                "Each critical group must be an array");
        }

        std::set<NodeID> group;
        for (auto const& nodeStr : groupValue)
        {
            if (!nodeStr.isString())
            {
                throw RustQuorumCheckerError("Each node ID must be a string");
            }
            group.insert(KeyUtils::fromStrKey<NodeID>(nodeStr.asString()));
        }
        criticalGroups.insert(std::move(group));
    }
}

QuorumCheckerStatus
fromQuorumCheckerStatusJson(Json::Value const& value)
{
    if (!value.isUInt())
    {
        throw RustQuorumCheckerError("Status must be an unsigned integer");
    }

    auto statusInt = value.asUInt();
    if (statusInt > static_cast<unsigned>(QuorumCheckerStatus::UNKNOWN))
    {
        throw RustQuorumCheckerError("Invalid status value: " +
                                     std::to_string(statusInt));
    }

    return static_cast<QuorumCheckerStatus>(statusInt);
}

void
writeResults(std::string const& outPath, QuorumCheckerStatus status,
             QuorumSplit const& split,
             std::set<std::set<NodeID>> const& criticalGroups,
             quorum_checker::QuorumCheckerMetrics metrics,
             std::string const& errorMsg)
{
    Json::Value results;
    results["status"] = static_cast<Json::UInt>(status);
    results["quorum_split"] = toQuorumSplitJson(split);
    results["intersection_critical_groups"] =
        toCriticalGroupsJson(criticalGroups);
    results["error"] = errorMsg;
    results["metrics"] = metrics.toJson();

    std::ofstream out(outPath, std::ios::out | std::ios::trunc);
    if (!out.is_open())
    {
        throw RustQuorumCheckerError("Failed to open output file: " + outPath);
    }
    out << results;
    if (out.fail())
    {
        throw RustQuorumCheckerError("Failed to write to output file: " +
                                     outPath);
    }
    out.flush();
    out.close();
}

Json::Value
parseResultsJson(std::string const& resultsJson)
{
    std::ifstream in(resultsJson);
    if (!in)
    {
        throw RustQuorumCheckerError("Could not open result file '" +
                                     resultsJson + "'");
    }
    Json::Reader reader;
    Json::Value res;
    if (!reader.parse(in, res) || !res.isObject())
    {
        throw RustQuorumCheckerError("Failed to parse result file '" +
                                     resultsJson + "' as a JSON object");
    }
    // Validate all required fields that correspond to writeResults
    if (!res.isMember("status") || !res["status"].isUInt())
    {
        throw RustQuorumCheckerError(
            "Result missing or invalid 'status' field");
    }
    if (!res.isMember("quorum_split"))
    {
        throw RustQuorumCheckerError("Result missing 'quorum_split' field");
    }
    if (!res.isMember("intersection_critical_groups"))
    {
        throw RustQuorumCheckerError(
            "Result missing 'intersection_critical_groups' field");
    }
    if (!res.isMember("error"))
    {
        throw RustQuorumCheckerError("Result missing 'error' field");
    }
    if (!res.isMember("metrics"))
    {
        throw RustQuorumCheckerError("Result missing 'metrics' field");
    }
    return res;
}
} // namespace

namespace stellar
{
namespace quorum_checker
{

QuorumCheckerMetrics::QuorumCheckerMetrics()
    : mSuccessfulRun(0)
    , mFailedRun(0)
    , mAbortedRun(0)
    , mResultPotentialSplit(0)
    , mResultUnknown(0)
    , mCumulativeTimeMs(0)
    , mCumulativeMemByte(0)
{
}

QuorumCheckerMetrics::QuorumCheckerMetrics(Json::Value const& value)
{
    if (!value.isObject())
    {
        throw RustQuorumCheckerError("Metrics JSON must be an object");
    }
    if (!value.isMember("successful_run_count") ||
        !value["successful_run_count"].isUInt())
    {
        throw RustQuorumCheckerError(
            "Metrics missing or invalid 'successful_run_count' field");
    }
    if (!value.isMember("failed_run_count") ||
        !value["failed_run_count"].isUInt())
    {
        throw RustQuorumCheckerError(
            "Metrics missing or invalid 'failed_run_count' field");
    }
    if (!value.isMember("aborted_run_count") ||
        !value["aborted_run_count"].isUInt())
    {
        throw RustQuorumCheckerError(
            "Metrics missing or invalid 'aborted_run_count' field");
    }
    if (!value.isMember("result_potential_split_count") ||
        !value["result_potential_split_count"].isUInt())
    {
        throw RustQuorumCheckerError(
            "Metrics missing or invalid 'result_potential_split_count' field");
    }
    if (!value.isMember("result_unknown_count") ||
        !value["result_unknown_count"].isUInt())
    {
        throw RustQuorumCheckerError(
            "Metrics missing or invalid 'result_unknown_count' field");
    }
    if (!value.isMember("cumulative_time_ms") ||
        !value["cumulative_time_ms"].isUInt64())
    {
        throw RustQuorumCheckerError(
            "Metrics missing or invalid 'cumulative_time_ms' field");
    }
    if (!value.isMember("cumulative_mem_byte") ||
        !value["cumulative_mem_byte"].isUInt64())
    {
        throw RustQuorumCheckerError(
            "Metrics missing or invalid 'cumulative_mem_byte' field");
    }
    mSuccessfulRun = value["successful_run_count"].asUInt64();
    mFailedRun = value["failed_run_count"].asUInt64();
    mAbortedRun = value["aborted_run_count"].asUInt64();
    mResultPotentialSplit = value["result_potential_split_count"].asUInt64();
    mResultUnknown = value["result_unknown_count"].asUInt64();
    mCumulativeTimeMs = value["cumulative_time_ms"].asUInt64();
    mCumulativeMemByte = value["cumulative_mem_byte"].asUInt64();
}

Json::Value
QuorumCheckerMetrics::toJson()
{
    Json::Value ret;
    ret["successful_run_count"] = Json::UInt64(mSuccessfulRun);
    ret["failed_run_count"] = Json::UInt64(mFailedRun);
    ret["aborted_run_count"] = Json::UInt64(mAbortedRun);
    ret["result_potential_split_count"] = Json::UInt64(mResultPotentialSplit);
    ret["result_unknown_count"] = Json::UInt64(mResultUnknown);
    ret["cumulative_time_ms"] = Json::UInt64(mCumulativeTimeMs);
    ret["cumulative_mem_byte"] = Json::UInt64(mCumulativeMemByte);
    return ret;
}

void
QuorumCheckerMetrics::flush(MetricsRegistry& metrics)
{
    metrics.NewCounter({"scp", "qic", "successful-run"}).inc(mSuccessfulRun);
    metrics.NewCounter({"scp", "qic", "failed-run"}).inc(mFailedRun);
    metrics.NewCounter({"scp", "qic", "aborted-run"}).inc(mAbortedRun);
    metrics.NewCounter({"scp", "qic", "result-potential-split"})
        .inc(mResultPotentialSplit);
    metrics.NewCounter({"scp", "qic", "result-unknown"}).inc(mResultUnknown);
    metrics.NewMeter({"scp", "qic", "cumulative-time-ms"}, "milli-second")
        .Mark(mCumulativeTimeMs);
    metrics.NewMeter({"scp", "qic", "cumulative-mem-byte"}, "byte")
        .Mark(mCumulativeMemByte);
    mSuccessfulRun = 0;
    mFailedRun = 0;
    mAbortedRun = 0;
    mResultPotentialSplit = 0;
    mResultUnknown = 0;
    mCumulativeTimeMs = 0;
    mCumulativeMemByte = 0;
}

static QuorumCheckerStatus
checkQuorumIntersectionInner(
    QuorumIntersectionChecker::QuorumSetMap const& qmap, QuorumSplit& split,
    QuorumCheckerResource& limits, QuorumCheckerMetrics& metrics)
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
                       KeyUtils::toStrKey(pair.first));
        }
    }

    QuorumCheckerResource usage{0, 0};
    try
    {
        QuorumCheckerStatus status;
        status = rust_bridge::network_enjoys_quorum_intersection(
            nodesBuf, quorumSetsBuf, split, limits, usage);
        CLOG_DEBUG(SCP,
                   "Quorum intersection checker used {} milliseconds and {} "
                   "bytes, returns status {}",
                   usage.time_ms, usage.mem_bytes, (uint8_t)status);

        // update the cumulative metrics, this will be reported metrics
        metrics.mCumulativeTimeMs += usage.time_ms;
        metrics.mCumulativeMemByte += usage.mem_bytes;
        metrics.mSuccessfulRun += 1;
        if (status == QuorumCheckerStatus::UNKNOWN)
        {
            metrics.mResultUnknown += 1;
        }
        else if (status == QuorumCheckerStatus::SAT)
        {
            metrics.mResultPotentialSplit += 1;
        }

        // Update time limit. Memory is a transient resource that gets reclaimed
        // back.
        if (limits.time_ms < usage.time_ms)
        {
            limits.time_ms = 0;
        }
        else
        {
            limits.time_ms -= usage.time_ms;
        }
        return status;
    }
    catch (std::exception const& e)
    {
        metrics.mFailedRun += 1;
        metrics.mCumulativeTimeMs += usage.time_ms;
        metrics.mCumulativeMemByte += usage.mem_bytes;
        std::string msg = e.what();
        CLOG_ERROR(SCP, "Quorum intersection checker error: {}  \
                \n cumulative resource used {} ms, {} bytes",
                   msg, metrics.mCumulativeTimeMs, metrics.mCumulativeMemByte);
        throw RustQuorumCheckerError(e.what());
    }
}

QuorumCheckerStatus
networkEnjoysQuorumIntersection(std::string const& inJsonPath,
                                uint64_t timeLimitMs, size_t memoryLimitBytes,
                                bool analyzeCriticalGroups,
                                std::string const& outResultJsonPath)
{
    QuorumIntersectionChecker::QuorumSetMap qmap =
        parseQuorumMapFromJson(inJsonPath);

    QuorumCheckerStatus status = QuorumCheckerStatus::UNKNOWN;
    QuorumSplit split;
    std::set<std::set<NodeID>> criticalGroups;
    QuorumCheckerMetrics metrics;
    try
    {
        QuorumCheckerResource limits{timeLimitMs, memoryLimitBytes};
        status = checkQuorumIntersectionInner(qmap, split, limits, metrics);

        // only run critical analysis if quorum enjoys intersection (`UNSAT`).
        // otherwise we want to return as soon as possible.
        if (status == QuorumCheckerStatus::UNSAT && analyzeCriticalGroups)
        {
            auto cb = [&](QuorumIntersectionChecker::QuorumSetMap const& qmap,
                          std::optional<stellar::Config> const& _cfg) -> bool {
                QuorumSplit _potentialSplit;
                return checkQuorumIntersectionInner(qmap, _potentialSplit,
                                                    limits, metrics) ==
                       QuorumCheckerStatus::UNSAT;
            };
            criticalGroups =
                QuorumIntersectionChecker::getIntersectionCriticalGroups(
                    qmap, std::nullopt, cb);
        }
        writeResults(outResultJsonPath, status, split, criticalGroups, metrics,
                     "");
    }
    catch (std::exception const& e)
    {
        std::string msg = e.what();
        writeResults(outResultJsonPath, status, split, criticalGroups, metrics,
                     e.what());
    }
    return status;
}

void
runQuorumIntersectionCheckAsync(
    Application& app, Hash const curr, uint32 ledger,
    std::string const& tmpDirName, QuorumTracker::QuorumMap const& qmap,
    std::weak_ptr<QuorumMapIntersectionState> hState, ProcessManager& pm,
    uint64_t timeLimitMs, size_t memoryLimitBytes, bool analyzeCriticalGroups)
{
    ZoneScoped;

    std::string quorumMapFile(fmt::format("{}/quorum_map.json", tmpDirName));
    std::string qicOutFile(fmt::format("{}/out.txt", tmpDirName)); // for stdout
    std::string qicResultJson(
        fmt::format("{}/result.json", tmpDirName)); // for result

    auto qmapJson = toQuorumMapJson(qmap);
    bool wroteQuorumMapFile = false;
    std::ofstream out(quorumMapFile);
    if (out.is_open())
    {
        out << qmapJson;
        if (out.fail())
        {
            CLOG_ERROR(SCP, "Failed to write quorum map file: {}",
                       quorumMapFile);
        }
        else
        {
            out.flush();
            if (out.fail())
            {
                CLOG_ERROR(SCP, "Failed to flush quorum map file: {}",
                           quorumMapFile);
            }
            else
            {
                out.close();
                if (out.fail())
                {
                    CLOG_ERROR(SCP, "Failed to close quorum map file: {}",
                               quorumMapFile);
                }
                else
                {
                    wroteQuorumMapFile = true;
                }
            }
        }
    }
    else
    {
        CLOG_ERROR(SCP, "Failed to open quorum map file: {}", quorumMapFile);
    }
    if (!wroteQuorumMapFile)
    {
        auto hStateSP = hState.lock();
        if (hStateSP)
        {
            hStateSP->reset(app);
        }
        return;
    }
    size_t numNodes = qmap.size();

    // We propagate the logging level of partition "SCP" because that's the
    // level which the quorum checker is assigned under. This means the
    // subprocess will be globally logging at this level.
    std::string exe = std::string(rust_bridge::current_exe());
    std::string ll = Logging::getStringFromLL(Logging::getLogLevel("SCP"));
    std::string cmdline = fmt::format(
        "{} check-quorum-intersection {} --ll {} --result-json {} "
        "--time-limit-ms {} --memory-limit-bytes {} {}",
        exe, quorumMapFile, ll, qicResultJson, timeLimitMs, memoryLimitBytes,
        analyzeCriticalGroups ? "--analyze-critical-groups" : "");
    auto evt = pm.runProcess(cmdline, qicOutFile).lock();

    if (!evt)
    {
        CLOG_ERROR(SCP, "Failed to start quorum intersection check process");
        auto hStateSP = hState.lock();
        if (hStateSP)
        {
            hStateSP->reset(app);
        }
        return;
    }

    evt->async_wait([numNodes, ledger, curr, qicOutFile, qicResultJson, hState,
                     &app](asio::error_code ec) {
        auto hStateSP = hState.lock();
        if (hStateSP == nullptr)
        {
            // hState being expired means the herder has initiated shutdown
            // process we do nothing and return
            return;
        }

        // Note: the ecode should match the return code from the command
        // line-running process which is just `QuorumCheckerStatus` as integer
        // on success. However, if the command fails due to abort (if exceeding
        // the memory limit), the ecode=1 will be returned because of the
        // simplification of collapsing all non-WIFEXITED exits to error code 1
        // (see `mapExitStatusToErrorCode` in ProcessManagerImpl.cpp).
        int ecode = ec.value();
        CLOG_DEBUG(SCP,
                   "Processing quorum intersection check result: numNodes={}, "
                   "ledger={}, "
                   "hash={}, outFile='{}', resultJson='{}', errorCode = {}",
                   numNodes, ledger, binToHex(curr), qicOutFile, qicResultJson,
                   ecode);

        if (ecode == static_cast<int>(QuorumCheckerStatus::UNSAT) ||
            ecode == static_cast<int>(QuorumCheckerStatus::SAT) ||
            ecode == static_cast<int>(QuorumCheckerStatus::UNKNOWN))
        {
            try
            {
                auto res = parseResultsJson(qicResultJson);
                hStateSP->mStatus = fromQuorumCheckerStatusJson(res["status"]);
                QuorumCheckerMetrics metrics(res["metrics"]);
                metrics.flush(hStateSP->mMetrics);
                // only update the following info if we had a complete run
                if (ecode == static_cast<int>(QuorumCheckerStatus::UNSAT) ||
                    ecode == static_cast<int>(QuorumCheckerStatus::SAT))
                {
                    hStateSP->mNumNodes = numNodes;
                    hStateSP->mLastCheckLedger = ledger;
                    hStateSP->mLastCheckQuorumMapHash = curr;
                    fromQuorumSplitJson(hStateSP->mPotentialSplit,
                                        res["quorum_split"]);
                    fromCriticalGroupsJson(hStateSP->mIntersectionCriticalNodes,
                                           res["intersection_critical_groups"]);
                    if (hStateSP->mStatus == QuorumCheckerStatus::UNSAT)
                    {
                        hStateSP->mLastGoodLedger = ledger;
                    }
                }
            }
            catch (RustQuorumCheckerError const& e)
            {
                CLOG_ERROR(SCP,
                           "Error processing quorum intersection result: {}",
                           e.what());
            }
        }
        else
        {
            hStateSP->mMetrics.NewCounter({"scp", "qic", "aborted-run"}).inc();
            CLOG_ERROR(SCP, "quorum intersection command failed, rc = {}",
                       ecode);
        }
        hStateSP->reset(app);
    });
}

} // namespace quorum_checker
} // namespace stellar
