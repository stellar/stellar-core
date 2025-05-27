// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/RustQuorumCheckerAdaptor.h"
#include "crypto/KeyUtils.h"
#include "herder/HerderImpl.h"
#include "herder/HerderUtils.h"
#include "herder/QuorumIntersectionChecker.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
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
    for (const auto& nodeStr : value[0])
    {
        if (!nodeStr.isString())
        {
            throw RustQuorumCheckerError("Each node ID must be a string");
        }
        split.first.emplace_back(
            KeyUtils::fromStrKey<NodeID>(nodeStr.asString()));
    }

    // Parse right side
    for (const auto& nodeStr : value[1])
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
    for (const auto& groupValue : value)
    {
        if (!groupValue.isArray())
        {
            throw RustQuorumCheckerError(
                "Each critical group must be an array");
        }

        std::set<NodeID> group;
        for (const auto& nodeStr : groupValue)
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
validateResultJson(Json::Value const& res)
{
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
}
} // namespace

namespace stellar
{
namespace quorum_checker
{

QuorumCheckerMetrics::QuorumCheckerMetrics()
    : mSuccessfulCalls(0)
    , mFailedCalls(0)
    , mInterruptedCalls(0)
    , mPotentialSplits(0)
    , mCumulativeTimeMs(0)
    , mCumulativeMemBytes(0)
{
}

QuorumCheckerMetrics::QuorumCheckerMetrics(Json::Value const& value)
{
    if (!value.isObject())
    {
        throw RustQuorumCheckerError("Metrics JSON must be an object");
    }
    if (!value.isMember("successful_call_count") ||
        !value["successful_call_count"].isUInt())
    {
        throw RustQuorumCheckerError(
            "Metrics missing or invalid 'successful_call_count' field");
    }
    if (!value.isMember("failed_call_count") ||
        !value["failed_call_count"].isUInt())
    {
        throw RustQuorumCheckerError(
            "Metrics missing or invalid 'failed_call_count' field");
    }
    if (!value.isMember("interrupted_call_count") ||
        !value["interrupted_call_count"].isUInt())
    {
        throw RustQuorumCheckerError(
            "Metrics missing or invalid 'interrupted_call_count' field");
    }
    if (!value.isMember("potential_split_count") ||
        !value["potential_split_count"].isUInt())
    {
        throw RustQuorumCheckerError(
            "Metrics missing or invalid 'potential_split_count' field");
    }
    if (!value.isMember("cumulative_time_ms") ||
        !value["cumulative_time_ms"].isUInt64())
    {
        throw RustQuorumCheckerError(
            "Metrics missing or invalid 'cumulative_time_ms' field");
    }
    if (!value.isMember("cumulative_memory_bytes") ||
        !value["cumulative_memory_bytes"].isUInt64())
    {
        throw RustQuorumCheckerError(
            "Metrics missing or invalid 'cumulative_memory_bytes' field");
    }
    mSuccessfulCalls = value["successful_call_count"].asUInt64();
    mFailedCalls = value["failed_call_count"].asUInt64();
    mInterruptedCalls = value["interrupted_call_count"].asUInt64();
    mPotentialSplits = value["potential_split_count"].asUInt64();
    mCumulativeTimeMs = value["cumulative_time_ms"].asUInt64();
    mCumulativeMemBytes = value["cumulative_memory_bytes"].asUInt64();
}

Json::Value
QuorumCheckerMetrics::toJson()
{
    Json::Value ret;
    ret["successful_call_count"] = Json::UInt64(mSuccessfulCalls);
    ret["failed_call_count"] = Json::UInt64(mFailedCalls);
    ret["interrupted_call_count"] = Json::UInt64(mInterruptedCalls);
    ret["potential_split_count"] = Json::UInt64(mPotentialSplits);
    ret["cumulative_time_ms"] = Json::UInt64(mCumulativeTimeMs);
    ret["cumulative_memory_bytes"] = Json::UInt64(mCumulativeMemBytes);
    return ret;
}

void
QuorumCheckerMetrics::flush(medida::MetricsRegistry& metrics)
{
    metrics.NewCounter({"scp", "qic", "successful-calls"})
        .inc(mSuccessfulCalls);
    metrics.NewCounter({"scp", "qic", "failed-calls"}).inc(mFailedCalls);
    metrics.NewCounter({"scp", "qic", "interrupted-calls"})
        .inc(mInterruptedCalls);
    metrics.NewCounter({"scp", "qic", "potential-splits"})
        .inc(mPotentialSplits);
    metrics.NewMeter({"scp", "qic", "cumulative-time-ms"}, "milli-second")
        .Mark(mCumulativeTimeMs);
    metrics.NewMeter({"scp", "qic", "cumulative-memory-bytes"}, "byte")
        .Mark(mCumulativeMemBytes);
    mSuccessfulCalls = 0;
    mFailedCalls = 0;
    mInterruptedCalls = 0;
    mPotentialSplits = 0;
    mCumulativeTimeMs = 0;
    mCumulativeMemBytes = 0;
}

QuorumCheckerStatus
checkQuorumIntersectionInner(
    QuorumIntersectionChecker::QuorumSetMap const& qmap, QuorumSplit& split,
    QuorumCheckerResource& available, QuorumCheckerMetrics& metrics)
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

    try
    {
        QuorumCheckerStatus status;
        QuorumCheckerResource usage;
        status = rust_bridge::network_enjoys_quorum_intersection(
            nodesBuf, quorumSetsBuf, split, available, usage);
        CLOG_DEBUG(SCP,
                   "Quorum intersection checker used {} milliseconds and {} "
                   "bytes, returns status {}",
                   usage.time_ms, usage.mem_bytes, (uint8_t)status);

        // update the cumulative metrics, this will be reported metrics
        metrics.mCumulativeTimeMs += usage.time_ms;
        metrics.mCumulativeMemBytes += usage.mem_bytes;
        metrics.mSuccessfulCalls += 1;
        if (status == QuorumCheckerStatus::UNKNOWN)
        {
            metrics.mInterruptedCalls += 1;
        }
        else if (status == QuorumCheckerStatus::SAT)
        {
            metrics.mPotentialSplits += 1;
        }

        // Update time limit. Memory is a transient resource that gets reclaimed
        // back.
        if (available.time_ms < usage.time_ms)
        {
            available.time_ms = 0;
        }
        else
        {
            available.time_ms -= usage.time_ms;
        }
        return status;
    }
    catch (const std::exception& e)
    {
        // internal rust solver error, or it has panicked
        metrics.mFailedCalls += 1;
        throw RustQuorumCheckerError(e.what());
    }
}

QuorumCheckerStatus
networkEnjoysQuorumIntersection(std::string const& inJsonPath,
                                uint64_t timeLimitMs, size_t memoryLimitBytes,
                                bool analyzeCriticalGroups,
                                std::string const& outResultJsonPath)
{
    QuorumCheckerStatus status{QuorumCheckerStatus::UNKNOWN};
    QuorumIntersectionChecker::QuorumSetMap qmap =
        parseQuorumMapFromJson(inJsonPath);

    Json::Value ret;
    try
    {
        QuorumSplit split;
        std::set<std::set<NodeID>> criticalGroups;
        QuorumCheckerResource limits{timeLimitMs, memoryLimitBytes};
        QuorumCheckerMetrics metrics;
        status = checkQuorumIntersectionInner(qmap, split, limits, metrics);

        // only run critical analysis if quorum enjoys intersection (`UNSAT`).
        // otherwise we want to return as soon as possible.
        if (status == QuorumCheckerStatus::UNSAT && analyzeCriticalGroups)
        {
            auto cb = [&](QuorumIntersectionChecker::QuorumSetMap const& qmap,
                          std::optional<stellar::Config> const& _cfg) -> bool {
                QuorumSplit _potentialSplit;
                QuorumCheckerStatus status = checkQuorumIntersectionInner(
                    qmap, _potentialSplit, limits, metrics);
                return status == QuorumCheckerStatus::UNSAT;
            };
            auto criticalGroups =
                QuorumIntersectionChecker::getIntersectionCriticalGroups(
                    qmap, std::nullopt, cb);
        }

        // write results to output json file
        std::ofstream out(outResultJsonPath, std::ios::out | std::ios::trunc);
        if (!out.is_open())
        {
            throw RustQuorumCheckerError("Failed to open output file: " +
                                         outResultJsonPath);
        }
        ret["status"] = static_cast<Json::UInt>(status);
        ret["quorum_split"] = toQuorumSplitJson(split);
        ret["intersection_critical_groups"] =
            toCriticalGroupsJson(criticalGroups);
        ret["metrics"] = metrics.toJson();

        out << ret;
        if (out.fail())
        {
            throw RustQuorumCheckerError("Failed to write to output file: " +
                                         outResultJsonPath);
        }
        out.flush();
        out.close();
    }
    catch (const std::exception& e)
    {
        ret["error"] = e.what();
        throw RustQuorumCheckerError(e.what());
    }
    return status;
}

void
runQuorumIntersectionCheckAsync(
    Hash const curr, uint32 ledger, std::string const& tmpDirName,
    QuorumTracker::QuorumMap const& qmap,
    std::weak_ptr<QuorumMapIntersectionState> hState, ProcessManager& pm,
    uint64_t timeLimitMs, size_t memoryLimitBytes, bool analyzeCriticalGroups)
{
    ZoneScoped;

    std::string quorumMapFile(fmt::format("{}/quorum_map.json", tmpDirName));
    std::string qicOutFile(fmt::format("{}/out.txt", tmpDirName)); // for stdout
    std::string qicResultJson(
        fmt::format("{}/result.json", tmpDirName)); // for result

    auto qmapJson = toQuorumMapJson(qmap);
    std::ofstream out(quorumMapFile);
    out << qmapJson;
    out.flush();
    out.close();
    size_t numNodes = qmap.size();

    // we propagate the logging level of partition "SCP" beacuse that's the
    // level which the quorum checker is assigned under. this means the
    // subprocess will be globally logging at this level.
    std::string exe = std::string(rust_bridge::current_exe());
    std::string ll = Logging::getStringFromLL(Logging::getLogLevel("SCP"));
    std::string cmdline = fmt::format(
        "{} check-quorum-intersection {} --ll {} --result-json {} "
        "--time-limit-ms {} --memory-limit-bytes {} {}",
        exe, quorumMapFile, ll, qicResultJson, timeLimitMs, memoryLimitBytes,
        analyzeCriticalGroups ? "--analyze-critical-groups" : "");
    auto evt = pm.runProcess(cmdline, qicOutFile).lock();

    evt->async_wait([numNodes, ledger, curr, qicOutFile, qicResultJson,
                     hState](asio::error_code ec) {
        auto hStateSP = hState.lock();
        if (hStateSP == nullptr)
        {
            // hState being expired means the herder has initiated shutdown
            // process we do nothing and return
            return;
        }

        int ecode = ec.value();
        CLOG_DEBUG(SCP,
                   "Processing quorum intersection check result: numNodes={}, "
                   "ledger={}, "
                   "hash={}, outFile='{}', resultJson='{}', errorCode = {}",
                   numNodes, ledger, binToHex(curr), qicOutFile, qicResultJson,
                   ecode);

        if (ecode == static_cast<int>(QuorumCheckerStatus::UNSAT) ||
            ecode == static_cast<int>(QuorumCheckerStatus::SAT))
        {
            std::ifstream in(qicResultJson);
            if (!in)
            {
                throw RustQuorumCheckerError("Could not open result file '" +
                                             qicResultJson + "'");
            }
            Json::Reader reader;
            Json::Value res;
            if (!reader.parse(in, res) || !res.isObject())
            {
                throw RustQuorumCheckerError("Failed to parse result file '" +
                                             qicResultJson +
                                             "' as a JSON object");
            }

            try
            {
                validateResultJson(res);

                hStateSP->mRecalculating = false;
                hStateSP->mNumNodes = numNodes;
                hStateSP->mLastCheckLedger = ledger;
                hStateSP->mLastCheckQuorumMapHash = curr;
                hStateSP->mCheckingQuorumMapHash = Hash{};
                hStateSP->mStatus = fromQuorumCheckerStatusJson(res["status"]);
                QuorumCheckerMetrics metrics(res["metrics"]);
                metrics.flush(hStateSP->mMetrics);
                fromQuorumSplitJson(hStateSP->mPotentialSplit,
                                    res["quorum_split"]);
                fromCriticalGroupsJson(hStateSP->mIntersectionCriticalNodes,
                                       res["intersection_critical_groups"]);
                if (hStateSP->mStatus == QuorumCheckerStatus::UNSAT)
                {
                    hStateSP->mLastGoodLedger = ledger;
                }
            }
            catch (const RustQuorumCheckerError& e)
            {
                CLOG_ERROR(SCP,
                           "Error processing quorum intersection result: {}",
                           e.what());
                hStateSP->mCheckingQuorumMapHash = Hash{};
            }
        }
        else
        {
            // interrupted or other errors
            hStateSP->mRecalculating = false;
            hStateSP->mCheckingQuorumMapHash = Hash{};
        }
    });
}

} // namespace quorum_checker
} // namespace stellar