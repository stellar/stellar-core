#include "simulation/ApplyLoad.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <unordered_map>

#include "bucket/BucketListSnapshot.h"
#include "bucket/BucketManager.h"
#include "bucket/test/BucketTestUtils.h"
#include "herder/Herder.h"
#include "herder/HerderImpl.h"
#include "herder/TxSetFrame.h"
#include "ledger/InMemorySorobanState.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerManagerImpl.h"
#include "main/Application.h"
#include "main/CommandLine.h"
#include "simulation/TxGenerator.h"
#include "test/TxTests.h"
#include "transactions/MutableTransactionResult.h"
#include "transactions/TransactionUtils.h"
#include "transactions/test/SorobanTxTestUtils.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/MetricsRegistry.h"
#include "util/XDRCereal.h"
#include "util/types.h"
#include "xdrpp/printer.h"
#include <crypto/SHA.h>

namespace stellar
{
namespace
{
constexpr double NOISY_BINARY_SEARCH_CONFIDENCE = 0.99;

LedgerKey
makeSACBalanceKey(SCAddress const& sacContract, SCVal const& holderAddrVal)
{
    LedgerKey key(CONTRACT_DATA);
    key.contractData().contract = sacContract;
    key.contractData().key =
        txtest::makeVecSCVal({makeSymbolSCVal("Balance"), holderAddrVal});
    key.contractData().durability = ContractDataDurability::PERSISTENT;
    return key;
}

LedgerKey
makeTrustlineKey(PublicKey const& accountID, Asset const& asset)
{
    LedgerKey key(TRUSTLINE);
    key.trustLine().accountID = accountID;
    key.trustLine().asset = assetToTrustLineAsset(asset);
    return key;
}

void
logExecutionEnvironmentSnapshot(Config const& cfg)
{
    std::ostringstream versionInfo;
    writeVersionInfo(versionInfo);

    CLOG_INFO(Perf, "[Apply load] Core version info:\n{}", versionInfo.str());

    auto const& configSnapshot = cfg.getLoadedConfigToml();
    CLOG_INFO(Perf, "[Apply load] Loaded Core config snapshot:\n{}",
              configSnapshot);
}

double
interpolatePercentile(std::vector<double> const& sortedValues,
                      double percentile)
{
    releaseAssert(!sortedValues.empty());
    if (sortedValues.size() == 1)
    {
        return sortedValues.front();
    }

    releaseAssert(percentile >= 0.0 && percentile <= 100.0);
    double rank = percentile / 100.0 * (sortedValues.size() - 1);
    auto lo = static_cast<size_t>(std::floor(rank));
    auto hi = static_cast<size_t>(std::ceil(rank));
    double weight = rank - lo;
    return sortedValues[lo] * (1.0 - weight) + sortedValues[hi] * weight;
}

struct PhaseStats
{
    double mean = 0;
    double stddev = 0;
    double p25 = 0;
    double median = 0;
    double p75 = 0;
    double p95 = 0;
    double p99 = 0;
};

PhaseStats
computePhaseStats(std::vector<double>& values)
{
    PhaseStats s;
    if (values.empty())
    {
        return s;
    }
    double sum = std::accumulate(values.begin(), values.end(), 0.0);
    s.mean = sum / values.size();
    double varianceSum = 0.0;
    for (auto v : values)
    {
        double d = v - s.mean;
        varianceSum += d * d;
    }
    s.stddev = std::sqrt(varianceSum / values.size());
    std::sort(values.begin(), values.end());
    s.p25 = interpolatePercentile(values, 25.0);
    s.median = interpolatePercentile(values, 50.0);
    s.p75 = interpolatePercentile(values, 75.0);
    s.p95 = interpolatePercentile(values, 95.0);
    s.p99 = interpolatePercentile(values, 99.0);
    return s;
}

void
logPhaseTimingsTable(
    std::vector<LedgerManagerImpl::LedgerClosePhaseTimings> const& allTimings)
{
    if (allTimings.empty())
    {
        return;
    }
    // Extract per-phase vectors.
    size_t n = allTimings.size();

    // Helper to extract a field into a vector.
    auto extract = [&](auto field) {
        std::vector<double> v(n);
        for (size_t i = 0; i < n; ++i)
        {
            v[i] = allTimings[i].*field;
        }
        return v;
    };

    auto prepareTxSet =
        extract(&LedgerManagerImpl::LedgerClosePhaseTimings::prepareTxSetMs);
    auto prefetchSrc = extract(
        &LedgerManagerImpl::LedgerClosePhaseTimings::prefetchSourceAccountsMs);
    auto feesSeqNums = extract(
        &LedgerManagerImpl::LedgerClosePhaseTimings::processFeesSeqNumsMs);
    auto applyTxs = extract(
        &LedgerManagerImpl::LedgerClosePhaseTimings::applyTransactionsMs);
    auto applyTxSetup =
        extract(&LedgerManagerImpl::LedgerClosePhaseTimings::applyTxSetupMs);
    auto prefetchTxData =
        extract(&LedgerManagerImpl::LedgerClosePhaseTimings::prefetchTxDataMs);
    auto applyTxMidSetup =
        extract(&LedgerManagerImpl::LedgerClosePhaseTimings::applyTxMidSetupMs);
    auto loadSorobanConfig = extract(
        &LedgerManagerImpl::LedgerClosePhaseTimings::loadSorobanConfigMs);
    auto buildTxBundles =
        extract(&LedgerManagerImpl::LedgerClosePhaseTimings::buildTxBundlesMs);
    auto sorobanSetupGlobal = extract(
        &LedgerManagerImpl::LedgerClosePhaseTimings::sorobanSetupGlobalMs);
    auto sorobanParallel = extract(
        &LedgerManagerImpl::LedgerClosePhaseTimings::sorobanParallelApplyMs);
    auto sorobanCheckInvariants = extract(
        &LedgerManagerImpl::LedgerClosePhaseTimings::sorobanCheckInvariantsMs);
    auto sorobanCommitThreads =
        extract(&LedgerManagerImpl::LedgerClosePhaseTimings::
                    sorobanCommitFromThreadsMs);
    auto sorobanDestroyThreads =
        extract(&LedgerManagerImpl::LedgerClosePhaseTimings::
                    sorobanDestroyThreadStatesMs);
    auto sorobanCommitLtx = extract(
        &LedgerManagerImpl::LedgerClosePhaseTimings::sorobanCommitToLtxMs);
    auto sorobanDestroyGlobal =
        extract(&LedgerManagerImpl::LedgerClosePhaseTimings::
                    sorobanDestroyGlobalStateMs);
    auto parTotal = extract(
        &LedgerManagerImpl::LedgerClosePhaseTimings::applyParallelPhaseTotalMs);
    auto applySeqClassic =
        extract(&LedgerManagerImpl::LedgerClosePhaseTimings::applySeqClassicMs);
    auto postTxSetApply =
        extract(&LedgerManagerImpl::LedgerClosePhaseTimings::postTxSetApplyMs);
    auto applyTxTail =
        extract(&LedgerManagerImpl::LedgerClosePhaseTimings::applyTxTailMs);
    auto destroyApplyStages = extract(
        &LedgerManagerImpl::LedgerClosePhaseTimings::destroyApplyStagesMs);
    auto upgrades =
        extract(&LedgerManagerImpl::LedgerClosePhaseTimings::applyUpgradesMs);
    auto sealBucket =
        extract(&LedgerManagerImpl::LedgerClosePhaseTimings::sealAndBucketMs);
    auto sqlCommit =
        extract(&LedgerManagerImpl::LedgerClosePhaseTimings::sqlCommitMs);
    auto postCommit =
        extract(&LedgerManagerImpl::LedgerClosePhaseTimings::postCommitMs);

    // Compute per-ledger gap inside parallel_total:
    //   parallel_total - sum(all sub-phases including destructors)
    std::vector<double> parGap(n);
    for (size_t i = 0; i < n; ++i)
    {
        parGap[i] = parTotal[i] - buildTxBundles[i] - sorobanSetupGlobal[i] -
                    sorobanParallel[i] - sorobanCheckInvariants[i] -
                    sorobanCommitThreads[i] - sorobanDestroyThreads[i] -
                    sorobanCommitLtx[i] - sorobanDestroyGlobal[i];
    }
    // Compute per-ledger gap inside apply_transactions:
    //   apply_transactions - sum(all sub-phases including destructors)
    std::vector<double> txGap(n);
    for (size_t i = 0; i < n; ++i)
    {
        txGap[i] = applyTxs[i] - applyTxSetup[i] - prefetchTxData[i] -
                   applyTxMidSetup[i] - loadSorobanConfig[i] - parTotal[i] -
                   applySeqClassic[i] - postTxSetApply[i] - applyTxTail[i] -
                   destroyApplyStages[i];
    }

    struct PhaseRow
    {
        std::string name;
        PhaseStats stats;
    };

    // Hierarchical layout:
    //   Level 0: top-level phases (no indent)
    //   Level 1: children of apply_transactions (2-space indent)
    //   Level 2: children of parallel_total (4-space indent)
    std::vector<PhaseRow> rows = {
        {"prepare_txset", computePhaseStats(prepareTxSet)},
        {"prefetch_src_accts", computePhaseStats(prefetchSrc)},
        {"process_fees_seqnums", computePhaseStats(feesSeqNums)},
        {"apply_transactions", computePhaseStats(applyTxs)},
        {"| setup", computePhaseStats(applyTxSetup)},
        {"| prefetch_tx_data", computePhaseStats(prefetchTxData)},
        {"| mid_setup", computePhaseStats(applyTxMidSetup)},
        {"| load_soroban_config", computePhaseStats(loadSorobanConfig)},
        {"| parallel_total", computePhaseStats(parTotal)},
        {"|   build_tx_bundles", computePhaseStats(buildTxBundles)},
        {"|   soroban_setup_glbl", computePhaseStats(sorobanSetupGlobal)},
        {"|   soroban_parallel", computePhaseStats(sorobanParallel)},
        {"|   soroban_invariants", computePhaseStats(sorobanCheckInvariants)},
        {"|   commit_from_thrds", computePhaseStats(sorobanCommitThreads)},
        {"|   ~thread_states", computePhaseStats(sorobanDestroyThreads)},
        {"|   commit_to_ltx", computePhaseStats(sorobanCommitLtx)},
        {"|   ~global_par_state", computePhaseStats(sorobanDestroyGlobal)},
        {"|   *** par gap ***", computePhaseStats(parGap)},
        {"| apply_seq_classic", computePhaseStats(applySeqClassic)},
        {"| post_tx_set_apply", computePhaseStats(postTxSetApply)},
        {"| tail", computePhaseStats(applyTxTail)},
        {"| ~apply_stages", computePhaseStats(destroyApplyStages)},
        {"| *** tx gap ***", computePhaseStats(txGap)},
        {"apply_upgrades", computePhaseStats(upgrades)},
        {"seal_and_bucket", computePhaseStats(sealBucket)},
        {"sql_commit", computePhaseStats(sqlCommit)},
        {"post_commit", computePhaseStats(postCommit)},
    };

    // Log the table header and rows.
    CLOG_WARNING(Perf,
                 "Phase timing breakdown ({} ledgers, all values in ms):", n);
    CLOG_WARNING(
        Perf, "{:<24s} {:>8s} {:>8s} {:>8s} {:>8s} {:>8s} {:>8s} {:>8s}",
        "phase", "mean", "stddev", "median", "p25", "p75", "p95", "p99");
    CLOG_WARNING(
        Perf,
        "{:-<24s} {:->8s} {:->8s} {:->8s} {:->8s} {:->8s} {:->8s} {:->8s}", "",
        "", "", "", "", "", "", "");
    for (auto const& r : rows)
    {
        CLOG_WARNING(Perf,
                     "{:<24s} {:>8.2f} {:>8.2f} {:>8.2f} {:>8.2f} {:>8.2f} "
                     "{:>8.2f} {:>8.2f}",
                     r.name, r.stats.mean, r.stats.stddev, r.stats.median,
                     r.stats.p25, r.stats.p75, r.stats.p95, r.stats.p99);
    }
}

void
logTxSetBuildTimingsTable(
    std::vector<TxSetBuildPhaseTimings> const& allTimings)
{
    if (allTimings.empty())
    {
        return;
    }

    size_t n = allTimings.size();
    auto extract = [&](auto field) {
        std::vector<double> v(n);
        for (size_t i = 0; i < n; ++i)
        {
            v[i] = allTimings[i].*field;
        }
        return v;
    };

    auto total = extract(&TxSetBuildPhaseTimings::totalMs);
    auto trimClassic = extract(&TxSetBuildPhaseTimings::trimInvalidClassicMs);
    auto surgeClassic =
        extract(&TxSetBuildPhaseTimings::surgePricingClassicMs);
    auto trimSoroban = extract(&TxSetBuildPhaseTimings::trimInvalidSorobanMs);
    auto surgeSoroban =
        extract(&TxSetBuildPhaseTimings::surgePricingSorobanMs);
    auto parallelBuild =
        extract(&TxSetBuildPhaseTimings::buildParallelSorobanPhaseMs);
    auto buildApplicable =
        extract(&TxSetBuildPhaseTimings::buildApplicableTxSetMs);
    auto toWire = extract(&TxSetBuildPhaseTimings::toWireTxSetMs);
    auto prepareForApply =
        extract(&TxSetBuildPhaseTimings::prepareTxSetForApplyMs);
    auto validateShape =
        extract(&TxSetBuildPhaseTimings::validateRoundTripShapeMs);
    auto validateTxSet = extract(&TxSetBuildPhaseTimings::validateTxSetMs);

    std::vector<double> classicTotal(n);
    std::vector<double> sorobanTotal(n);
    std::vector<double> sorobanSurgeGap(n);
    std::vector<double> totalGap(n);
    for (size_t i = 0; i < n; ++i)
    {
        classicTotal[i] = trimClassic[i] + surgeClassic[i];
        sorobanTotal[i] = trimSoroban[i] + surgeSoroban[i];
        sorobanSurgeGap[i] = surgeSoroban[i] - parallelBuild[i];
        totalGap[i] = total[i] - classicTotal[i] - sorobanTotal[i] -
                      buildApplicable[i] - toWire[i] - prepareForApply[i] -
                      validateShape[i] - validateTxSet[i];
    }

    struct PhaseRow
    {
        std::string name;
        PhaseStats stats;
    };

    std::vector<PhaseRow> rows = {
        {"total", computePhaseStats(total)},
        {"phase_classic", computePhaseStats(classicTotal)},
        {"| trim_invalid", computePhaseStats(trimClassic)},
        {"| surge_pricing", computePhaseStats(surgeClassic)},
        {"phase_soroban", computePhaseStats(sorobanTotal)},
        {"| trim_invalid", computePhaseStats(trimSoroban)},
        {"| surge_pricing", computePhaseStats(surgeSoroban)},
        {"|   parallel_build", computePhaseStats(parallelBuild)},
        {"|   *** soroban gap ***", computePhaseStats(sorobanSurgeGap)},
        {"build_applicable", computePhaseStats(buildApplicable)},
        {"to_wire", computePhaseStats(toWire)},
        {"prepare_for_apply", computePhaseStats(prepareForApply)},
        {"validate_shape", computePhaseStats(validateShape)},
        {"validate_txset", computePhaseStats(validateTxSet)},
        {"*** txset gap ***", computePhaseStats(totalGap)},
    };

    CLOG_WARNING(
        Perf,
        "Tx-set build timing breakdown ({} ledgers, all values in ms):", n);
    CLOG_WARNING(
        Perf, "{:<28s} {:>8s} {:>8s} {:>8s} {:>8s} {:>8s} {:>8s} {:>8s}",
        "phase", "mean", "stddev", "median", "p25", "p75", "p95",
        "p99");
    CLOG_WARNING(
        Perf,
        "{:-<28s} {:->8s} {:->8s} {:->8s} {:->8s} {:->8s} {:->8s} {:->8s}",
        "", "", "", "", "", "", "", "");
    for (auto const& r : rows)
    {
        CLOG_WARNING(Perf,
                     "{:<28s} {:>8.2f} {:>8.2f} {:>8.2f} {:>8.2f} {:>8.2f} "
                     "{:>8.2f} {:>8.2f}",
                     r.name, r.stats.mean, r.stats.stddev, r.stats.median,
                     r.stats.p25, r.stats.p75, r.stats.p95, r.stats.p99);
    }
}

SorobanUpgradeConfig
getUpgradeConfig(Config const& cfg, bool validate = true)
{
    SorobanUpgradeConfig upgradeConfig;
    upgradeConfig.maxContractSizeBytes = 65536;
    upgradeConfig.maxContractDataKeySizeBytes = 250;
    upgradeConfig.maxContractDataEntrySizeBytes = 65536;
    upgradeConfig.ledgerMaxInstructions =
        cfg.APPLY_LOAD_LEDGER_MAX_INSTRUCTIONS;
    upgradeConfig.txMaxInstructions = cfg.APPLY_LOAD_TX_MAX_INSTRUCTIONS;
    upgradeConfig.txMemoryLimit = 41943040;
    upgradeConfig.ledgerMaxDiskReadEntries =
        cfg.APPLY_LOAD_LEDGER_MAX_DISK_READ_LEDGER_ENTRIES;
    upgradeConfig.ledgerMaxDiskReadBytes =
        cfg.APPLY_LOAD_LEDGER_MAX_DISK_READ_BYTES;
    upgradeConfig.ledgerMaxWriteLedgerEntries =
        cfg.APPLY_LOAD_LEDGER_MAX_WRITE_LEDGER_ENTRIES;
    upgradeConfig.ledgerMaxWriteBytes = cfg.APPLY_LOAD_LEDGER_MAX_WRITE_BYTES;
    upgradeConfig.ledgerMaxTxCount = cfg.APPLY_LOAD_MAX_SOROBAN_TX_COUNT;
    upgradeConfig.txMaxDiskReadEntries =
        cfg.APPLY_LOAD_TX_MAX_DISK_READ_LEDGER_ENTRIES;
    upgradeConfig.txMaxFootprintEntries = cfg.APPLY_LOAD_TX_MAX_FOOTPRINT_SIZE;
    upgradeConfig.txMaxDiskReadBytes = cfg.APPLY_LOAD_TX_MAX_DISK_READ_BYTES;
    upgradeConfig.txMaxWriteLedgerEntries =
        cfg.APPLY_LOAD_TX_MAX_WRITE_LEDGER_ENTRIES;
    upgradeConfig.txMaxWriteBytes = cfg.APPLY_LOAD_TX_MAX_WRITE_BYTES;
    upgradeConfig.txMaxContractEventsSizeBytes =
        cfg.APPLY_LOAD_MAX_CONTRACT_EVENT_SIZE_BYTES;
    upgradeConfig.ledgerMaxTransactionsSizeBytes =
        cfg.APPLY_LOAD_MAX_LEDGER_TX_SIZE_BYTES;
    upgradeConfig.txMaxSizeBytes = cfg.APPLY_LOAD_MAX_TX_SIZE_BYTES;
    upgradeConfig.liveSorobanStateSizeWindowSampleSize = 30;
    upgradeConfig.evictionScanSize = 100000;
    upgradeConfig.startingEvictionScanLevel = 7;

    upgradeConfig.ledgerMaxDependentTxClusters =
        cfg.APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS;

    // Increase the default TTL and reduce the rent rate in order to avoid the
    // state archival and too high rent fees. The apply load test is generally
    // not concerned about the resource fees.
    upgradeConfig.minPersistentTTL = 1'000'000'000;
    upgradeConfig.minTemporaryTTL = 1'000'000'000;
    upgradeConfig.maxEntryTTL = 1'000'000'001;
    upgradeConfig.persistentRentRateDenominator = 1'000'000'000'000LL;
    upgradeConfig.tempRentRateDenominator = 1'000'000'000'000LL;

    // These values are set above using values from Config, so the assertions
    // will fail if the config file is missing any of these values.
    if (validate)
    {
        releaseAssert(*upgradeConfig.ledgerMaxInstructions > 0);
        releaseAssert(*upgradeConfig.ledgerMaxDiskReadEntries > 0);
        releaseAssert(*upgradeConfig.ledgerMaxDiskReadBytes > 0);
        releaseAssert(*upgradeConfig.ledgerMaxWriteLedgerEntries > 0);
        releaseAssert(*upgradeConfig.ledgerMaxWriteBytes > 0);
        releaseAssert(*upgradeConfig.ledgerMaxTransactionsSizeBytes > 0);
        releaseAssert(*upgradeConfig.ledgerMaxTxCount > 0);
        releaseAssert(*upgradeConfig.txMaxInstructions > 0);
        releaseAssert(*upgradeConfig.txMaxDiskReadEntries > 0);
        releaseAssert(*upgradeConfig.txMaxDiskReadBytes > 0);
        releaseAssert(*upgradeConfig.txMaxWriteLedgerEntries > 0);
        releaseAssert(*upgradeConfig.txMaxWriteBytes > 0);
        releaseAssert(*upgradeConfig.txMaxContractEventsSizeBytes > 0);
        releaseAssert(*upgradeConfig.txMaxSizeBytes > 0);
    }
    return upgradeConfig;
}

SorobanUpgradeConfig
getUpgradeConfigForMaxTPS(Config const& cfg, uint64_t instructionsPerCluster,
                          uint32_t totalTxs)
{
    SorobanUpgradeConfig upgradeConfig;
    upgradeConfig.ledgerMaxDependentTxClusters =
        cfg.APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS;

    // Set high limits to avoid resource constraints during testing
    constexpr uint32_t LEDGER_MAX_LIMIT = UINT32_MAX / 2;
    constexpr uint32_t TX_MAX_LIMIT = UINT32_MAX / 4;

    upgradeConfig.maxContractSizeBytes = LEDGER_MAX_LIMIT;
    upgradeConfig.maxContractDataKeySizeBytes = LEDGER_MAX_LIMIT;
    upgradeConfig.maxContractDataEntrySizeBytes = LEDGER_MAX_LIMIT;
    upgradeConfig.txMemoryLimit = LEDGER_MAX_LIMIT;
    upgradeConfig.evictionScanSize = 100;
    upgradeConfig.startingEvictionScanLevel = 7;

    upgradeConfig.ledgerMaxDiskReadEntries = LEDGER_MAX_LIMIT;
    upgradeConfig.ledgerMaxDiskReadBytes = LEDGER_MAX_LIMIT;
    upgradeConfig.ledgerMaxWriteLedgerEntries = LEDGER_MAX_LIMIT;
    upgradeConfig.ledgerMaxWriteBytes = LEDGER_MAX_LIMIT;

    upgradeConfig.txMaxDiskReadEntries = TX_MAX_LIMIT;
    upgradeConfig.txMaxFootprintEntries = TX_MAX_LIMIT;
    upgradeConfig.txMaxDiskReadBytes = TX_MAX_LIMIT;
    upgradeConfig.txMaxWriteLedgerEntries = TX_MAX_LIMIT;
    upgradeConfig.txMaxWriteBytes = TX_MAX_LIMIT;

    upgradeConfig.ledgerMaxTransactionsSizeBytes = LEDGER_MAX_LIMIT;
    upgradeConfig.txMaxSizeBytes = TX_MAX_LIMIT;
    upgradeConfig.txMaxContractEventsSizeBytes = TX_MAX_LIMIT;

    // Increase the default TTL and reduce the rent rate in order to avoid the
    // state archival and too high rent fees. The apply load test is generally
    // not concerned about the resource fees.
    upgradeConfig.minPersistentTTL = 1'000'000'000;
    upgradeConfig.minTemporaryTTL = 1'000'000'000;
    upgradeConfig.maxEntryTTL = 1'000'000'001;
    upgradeConfig.persistentRentRateDenominator = 1'000'000'000'000LL;
    upgradeConfig.tempRentRateDenominator = 1'000'000'000'000LL;

    // Set the instruction and max tx count just high enough so that we generate
    // a full ledger. This ensures all available clusters are filled for maximum
    // parallelism.
    upgradeConfig.ledgerMaxInstructions = instructionsPerCluster;
    upgradeConfig.ledgerMaxTxCount = totalTxs;
    upgradeConfig.txMaxInstructions = instructionsPerCluster;

    releaseAssert(*upgradeConfig.ledgerMaxInstructions > 0);
    releaseAssert(*upgradeConfig.txMaxInstructions > 0);

    if (*upgradeConfig.ledgerMaxInstructions < *upgradeConfig.txMaxInstructions)
    {
        throw std::runtime_error("TPS too low, cannot achieve parallelism");
    }

    return upgradeConfig;
}
} // namespace

/*
 * Binary search for a noisy monotone function.
 *
 * This function locates an integer x* such that:
 *
 *     E[f(x*)] == targetA
 *
 * under the assumptions that:
 *   - f(x) is strictly monotone in x
 *   - evaluations of f(x) are noisy
 *   - the noise distribution and variance are unknown
 *
 * The algorithm performs adaptive binary search:
 *   - at each midpoint, samples until confident about the direction
 *   - uses t-statistics to determine if mean is above/below target
 *   - adjusts per-decision confidence to achieve overall confidence
 *
 * Parameters:
 * ----------
 * f :
 *     Expensive benchmark or measurement function.
 *     Must be monotone in x.
 *     Returns a noisy scalar measurement.
 *
 * targetA :
 *     Target value such that x* satisfies E[f(x*)] == targetA.
 *
 * xMin :
 *     Inclusive lower bound of the search domain.
 *
 * xMax :
 *     Inclusive upper bound of the search domain.
 *
 * confidence :
 *     Desired confidence level for the final result.
 *     Example: 0.95 means 95% probability the true x* is in [lo, hi].
 *     The algorithm computes per-decision confidence as confidence^(1/k)
 *     where k is the number of binary search decisions, ensuring the
 *     product of all decision confidences meets the overall target.
 *
 * xTolerance :
 *     Early-stop threshold on interval width.
 *     Search stops when (hi - lo) <= xTolerance.
 *     Use 0 to require a single integer solution.
 *
 * maxSamplesPerPoint :
 *     Maximum samples to take at each midpoint before giving up on confidence.
 *
 * prepareIteration :
 *     When set, call before sampling f at each midpoint.
 *
 * iterationResult :
 *     When set, call after iterations are done with a bool indicating whether
 *     the midpoint was confidently above (true) or below (false) the target.
 *
 * Returns:
 * --------
 * A pair (lo, hi) representing the search interval such that with
 * probability >= confidence, the true x* lies within [lo, hi].
 * The bounds are inclusive.
 */
#ifndef BUILD_TESTS
static std::pair<uint32_t, uint32_t>
#else
std::pair<uint32_t, uint32_t>
#endif
noisyBinarySearch(std::function<double(uint32_t)> const& f, double targetA,
                  uint32_t xMin, uint32_t xMax, double confidence,
                  uint32_t xTolerance, size_t maxSamplesPerPoint,
                  std::function<void(uint32_t)> const& prepareIteration,
                  std::function<void(uint32_t, bool)> const& iterationResult)
{
    releaseAssert(xMin <= xMax);
    size_t const minSamples = 30;
    releaseAssert(maxSamplesPerPoint >= minSamples);

    // Binary search bounds
    uint32_t lo = xMin;
    uint32_t hi = xMax;

    // Calculate per-decision confidence needed to achieve final confidence.
    // With k decisions each having probability p of being correct,
    // P(all correct) = p^k >= confidence
    // => p >= confidence^(1/k)
    size_t rangeSize = static_cast<size_t>(xMax - xMin + 1);
    size_t numDecisions = static_cast<size_t>(std::ceil(
        std::log2(static_cast<double>(rangeSize) / (xTolerance + 1))));
    numDecisions = std::max(numDecisions, size_t{1});

    double perDecisionConfidence =
        std::pow(confidence, 1.0 / static_cast<double>(numDecisions));

    // Minimum samples before we start checking confidence

    size_t totalSamples = 0;

    while (hi - lo > xTolerance)
    {
        uint32_t mid = lo + (hi - lo) / 2;

        // Collect samples using Welford's algorithm
        size_t count = 0;
        double mean = 0.0;
        double m2 = 0.0;

        double probAbove = 0.5;
        bool confident = false;
        if (prepareIteration)
        {
            prepareIteration(mid);
        }
        while (count < maxSamplesPerPoint)
        {
            // Take a sample
            double y = f(mid);
            count++;
            totalSamples++;
            double delta = y - mean;
            mean += delta / count;
            double delta2 = y - mean;
            m2 += delta * delta2;

            if (count < minSamples)
            {
                continue;
            }

            // Compute t-statistic: t = (mean - target) / (s / sqrt(n))
            double variance = m2 / (count - 1);
            double sem = std::sqrt(variance / count);
            CLOG_INFO(Perf,
                      "noisy binary search:x={}, y={}, n={}, mean={:.4f}, "
                      "variance={:.4f}, sem={:.4f}",
                      mid, y, count, mean, variance, sem);
            // Avoid division by zero
            if (sem < 1e-10)
            {
                // Variance is essentially zero - mean is very stable
                probAbove = (mean > targetA) ? 1.0 - 1e-10 : 1e-10;
                confident = true;
                break;
            }

            double t = (mean - targetA) / sem;

            // Convert t-statistic to probability using normal approximation
            // (good enough with 30+ samples).
            probAbove = 0.5 * std::erfc(-t / std::sqrt(2.0));
            // Check if we have enough confidence to make a decision
            if (probAbove >= perDecisionConfidence ||
                probAbove <= (1.0 - perDecisionConfidence))
            {
                confident = true;
                break;
            }
        }

        if (!confident)
        {
            // Couldn't reach required confidence - log a warning
            // but still make a decision based on best estimate
            CLOG_WARNING(
                Perf,
                "Noisy binary search: couldn't reach {:.4f} confidence at "
                "x={} after {} samples (probAbove={:.4f})",
                perDecisionConfidence, mid, count, probAbove);
        }
        else
        {
            CLOG_INFO(Perf,
                      "Noisy binary search: at x={} took {} samples to reach "
                      "{:.4f} confidence (probAbove={:.4f})",
                      mid, count, perDecisionConfidence, probAbove);
        }

        if (iterationResult)
        {
            iterationResult(mid, probAbove >= 0.5);
        }
        // Make decision based on best estimate
        if (probAbove >= 0.5)
        {
            hi = mid;
        }
        else
        {
            lo = mid + 1;
        }
    }
    CLOG_INFO(Perf,
              "Noisy binary search completed {} total samples; final interval "
              "[{}, {}]",
              totalSamples, lo, hi);

    return {lo, hi};
}

uint64_t
ApplyLoad::calculateInstructionsPerTx() const
{
    switch (mModelTx)
    {
    case ApplyLoadModelTx::CUSTOM_TOKEN:
        return TxGenerator::CUSTOM_TOKEN_TX_INSTRUCTIONS;
    case ApplyLoadModelTx::SOROSWAP:
        return TxGenerator::SOROSWAP_SWAP_TX_INSTRUCTIONS;
    case ApplyLoadModelTx::SAC:
    {
        uint32_t batchSize = mApp.getConfig().APPLY_LOAD_BATCH_SAC_COUNT;
        if (batchSize > 1)
        {
            return batchSize * TxGenerator::BATCH_TRANSFER_TX_INSTRUCTIONS;
        }
        return TxGenerator::SAC_TX_INSTRUCTIONS;
    }
    }
    releaseAssertOrThrow(false);
    return 0;
}

uint32_t
ApplyLoad::calculateBenchmarkModelTxCount() const
{
    auto const& config = mApp.getConfig();
    releaseAssertOrThrow(config.APPLY_LOAD_BATCH_SAC_COUNT > 0);

    switch (mModelTx)
    {
    case ApplyLoadModelTx::SAC:
        // In benchmark mode APPLY_LOAD_MAX_SOROBAN_TX_COUNT means modeled SAC
        // transfers, while generation expects number of tx envelopes.
        releaseAssertOrThrow(config.APPLY_LOAD_MAX_SOROBAN_TX_COUNT %
                                 config.APPLY_LOAD_BATCH_SAC_COUNT ==
                             0);
        {
            auto benchmarkTxCount = config.APPLY_LOAD_MAX_SOROBAN_TX_COUNT /
                                    config.APPLY_LOAD_BATCH_SAC_COUNT;
            if (benchmarkTxCount <
                config.APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS)
            {
                throw std::runtime_error(
                    "For benchmark SAC mode, "
                    "APPLY_LOAD_MAX_SOROBAN_TX_COUNT / "
                    "APPLY_LOAD_BATCH_SAC_COUNT must be at least "
                    "APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS to satisfy "
                    "requested parallelism");
            }
            return benchmarkTxCount;
        }
    case ApplyLoadModelTx::CUSTOM_TOKEN:
        // No batching for custom token, one transfer per tx envelope
        return config.APPLY_LOAD_MAX_SOROBAN_TX_COUNT;
    case ApplyLoadModelTx::SOROSWAP:
        // No batching for Soroswap, one swap per tx envelope
        return config.APPLY_LOAD_MAX_SOROBAN_TX_COUNT;
    }
    releaseAssertOrThrow(false);
    return 0;
}

void
ApplyLoad::upgradeSettingsForMaxTPS(uint32_t txsToGenerate)
{
    // Calculate the actual instructions needed for all transactions. The
    // ledger max instructions is the total instruction count per cluster. In
    // order to have max parallelism, we want each cluster to be full, so
    // upgrade settings such that we have just enough capacity across all
    // clusters.

    uint64_t instructionsPerTx = calculateInstructionsPerTx();

    uint64_t totalInstructions =
        static_cast<uint64_t>(txsToGenerate) * instructionsPerTx;
    uint64_t instructionsPerCluster =
        totalInstructions /
        mApp.getConfig().APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS;

    // Ensure all transactions can fit
    instructionsPerCluster += instructionsPerTx - 1;

    auto upgradeConfig = getUpgradeConfigForMaxTPS(
        mApp.getConfig(), instructionsPerCluster, txsToGenerate);

    applyConfigUpgrade(upgradeConfig);
}

// Given an index, returns the LedgerKey for an archived entry that is
// pre-populated in the Hot Archive.
LedgerKey
ApplyLoad::getKeyForArchivedEntry(uint64_t index)
{

    static SCAddress const hotArchiveContractID = [] {
        SCAddress addr;
        addr.type(SC_ADDRESS_TYPE_CONTRACT);
        addr.contractId() = sha256("archived-entry");
        return addr;
    }();

    LedgerKey lk;
    lk.type(CONTRACT_DATA);
    lk.contractData().contract = hotArchiveContractID;
    lk.contractData().key.type(SCV_U64);
    lk.contractData().key.u64() = index;
    lk.contractData().durability = ContractDataDurability::PERSISTENT;
    return lk;
}

uint32_t
ApplyLoad::calculateRequiredHotArchiveEntries(ApplyLoadMode mode,
                                              Config const& cfg)
{
    // If no RO entries are configured, return 0
    if (cfg.APPLY_LOAD_NUM_DISK_READ_ENTRIES.empty())
    {
        return 0;
    }

    releaseAssertOrThrow(
        cfg.APPLY_LOAD_NUM_DISK_READ_ENTRIES.size() ==
        cfg.APPLY_LOAD_NUM_DISK_READ_ENTRIES_DISTRIBUTION.size());

    // Calculate mean disk reads per transaction
    double totalWeight = std::accumulate(
        cfg.APPLY_LOAD_NUM_DISK_READ_ENTRIES_DISTRIBUTION.begin(),
        cfg.APPLY_LOAD_NUM_DISK_READ_ENTRIES_DISTRIBUTION.end(), 0.0);
    double meanDiskReadsPerTx = 0.0;
    for (size_t i = 0; i < cfg.APPLY_LOAD_NUM_DISK_READ_ENTRIES.size(); ++i)
    {
        meanDiskReadsPerTx +=
            static_cast<double>(cfg.APPLY_LOAD_NUM_DISK_READ_ENTRIES[i]) *
            (cfg.APPLY_LOAD_NUM_DISK_READ_ENTRIES_DISTRIBUTION[i] /
             totalWeight);
    }

    // Calculate total expected disk reads
    double totalExpectedRestores = meanDiskReadsPerTx *
                                   cfg.APPLY_LOAD_MAX_SOROBAN_TX_COUNT *
                                   cfg.APPLY_LOAD_NUM_LEDGERS;
    // We technically can only actually perform totalExpectedRestores, but we
    // still need to create valid transactions in the 'mempool', so we need
    // to scale the expected number of restores by the transaction queue size.
    totalExpectedRestores *= cfg.SOROBAN_TRANSACTION_QUEUE_SIZE_MULTIPLIER;

    // In FIND_LIMITS_FOR_MODEL_TX mode, we perform a binary search that uses
    // new restores and thus we need to additionally scale the restores by
    // log2 of max tx count (which approximates the maximum number of binary
    // search iterations).
    if (mode == ApplyLoadMode::FIND_LIMITS_FOR_MODEL_TX)
    {
        totalExpectedRestores *= log2(cfg.APPLY_LOAD_MAX_SOROBAN_TX_COUNT);
    }

    // Add some generous buffer since actual distributions may vary.
    return totalExpectedRestores * 1.5;
}

ApplyLoad::ApplyLoad(Application& app)
    : mApp(app)
    , mMode(app.getConfig().APPLY_LOAD_MODE)
    , mModelTx(app.getConfig().APPLY_LOAD_MODEL_TX)
    , mTotalHotArchiveEntries(
          calculateRequiredHotArchiveEntries(mMode, app.getConfig()))
    , mTxCountUtilization(
          mApp.getMetrics().NewHistogram({"soroban", "apply-load", "tx-count"}))
    , mInstructionUtilization(mApp.getMetrics().NewHistogram(
          {"soroban", "apply-load", "instructions"}))
    , mTxSizeUtilization(
          mApp.getMetrics().NewHistogram({"soroban", "apply-load", "tx-size"}))
    , mDiskReadByteUtilization(mApp.getMetrics().NewHistogram(
          {"soroban", "apply-load", "disk-read-byte"}))
    , mWriteByteUtilization(mApp.getMetrics().NewHistogram(
          {"soroban", "apply-load", "write-byte"}))
    , mDiskReadEntryUtilization(mApp.getMetrics().NewHistogram(
          {"soroban", "apply-load", "disk-read-entry"}))
    , mWriteEntryUtilization(mApp.getMetrics().NewHistogram(
          {"soroban", "apply-load", "write-entry"}))
    , mTxGenerator(app, mTotalHotArchiveEntries)
{
    auto const& config = mApp.getConfig();

    // Basic input parameter validation - it's not comprehensive, but should
    // catch some simple misconfiguration cases.
    if (mMode == ApplyLoadMode::BENCHMARK_MODEL_TX)
    {
        if (mModelTx == ApplyLoadModelTx::SAC)
        {
            if (config.APPLY_LOAD_MAX_SOROBAN_TX_COUNT %
                    config.APPLY_LOAD_BATCH_SAC_COUNT !=
                0)
            {
                throw std::runtime_error(
                    "For benchmark APPLY_LOAD_MODEL_TX=sac, "
                    "APPLY_LOAD_MAX_SOROBAN_TX_COUNT must be divisible by "
                    "APPLY_LOAD_BATCH_SAC_COUNT");
            }
            auto benchmarkTxCount = config.APPLY_LOAD_MAX_SOROBAN_TX_COUNT /
                                    config.APPLY_LOAD_BATCH_SAC_COUNT;
            if (benchmarkTxCount <
                config.APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS)
            {
                throw std::runtime_error(
                    "For benchmark APPLY_LOAD_MODEL_TX=sac, "
                    "APPLY_LOAD_MAX_SOROBAN_TX_COUNT / "
                    "APPLY_LOAD_BATCH_SAC_COUNT must be at least "
                    "APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS to satisfy "
                    "requested parallelism");
            }
        }
    }
    // Noisy binary search-based modes require at least 30 ledgers to have
    // enough samples for statistics to be meaningful.
    if (mMode == ApplyLoadMode::MAX_SAC_TPS ||
        mMode == ApplyLoadMode::FIND_LIMITS_FOR_MODEL_TX)
    {

        if (config.APPLY_LOAD_NUM_LEDGERS < 30)
        {
            throw std::runtime_error(
                "APPLY_LOAD_NUM_LEDGERS must be at least 30");
        }
    }

    if (mMode == ApplyLoadMode::MAX_SAC_TPS &&
        config.APPLY_LOAD_MAX_SAC_TPS_MIN_TPS >
            config.APPLY_LOAD_MAX_SAC_TPS_MAX_TPS)
    {
        throw std::runtime_error(
            "APPLY_LOAD_MAX_SAC_TPS_MIN_TPS must not be greater than "
            "APPLY_LOAD_MAX_SAC_TPS_MAX_TPS for max_sac_tps mode");
    }

    switch (mMode)
    {
    case ApplyLoadMode::LIMIT_BASED:
    case ApplyLoadMode::FIND_LIMITS_FOR_MODEL_TX:
        mNumAccounts = config.APPLY_LOAD_MAX_SOROBAN_TX_COUNT *
                           config.SOROBAN_TRANSACTION_QUEUE_SIZE_MULTIPLIER +
                       config.APPLY_LOAD_CLASSIC_TXS_PER_LEDGER *
                           config.TRANSACTION_QUEUE_SIZE_MULTIPLIER +
                       2;
        break;
    case ApplyLoadMode::MAX_SAC_TPS:
        mNumAccounts = config.APPLY_LOAD_MAX_SAC_TPS_MAX_TPS *
                           config.SOROBAN_TRANSACTION_QUEUE_SIZE_MULTIPLIER *
                           config.APPLY_LOAD_TARGET_CLOSE_TIME_MS / 1000.0 +
                       config.APPLY_LOAD_CLASSIC_TXS_PER_LEDGER;
        break;
    case ApplyLoadMode::BENCHMARK_MODEL_TX:
        if (mModelTx == ApplyLoadModelTx::CUSTOM_TOKEN)
        {
            // Need 2 unique accounts per transfer to avoid conflicts
            mNumAccounts = config.APPLY_LOAD_MAX_SOROBAN_TX_COUNT * 2 +
                           config.APPLY_LOAD_CLASSIC_TXS_PER_LEDGER;
        }
        else if (mModelTx == ApplyLoadModelTx::SOROSWAP)
        {
            // Need 1 unique account per swap + classic accounts + root
            mNumAccounts = config.APPLY_LOAD_MAX_SOROBAN_TX_COUNT + 1 +
                           config.APPLY_LOAD_CLASSIC_TXS_PER_LEDGER;
        }
        else
        {
            mNumAccounts =
                config.APPLY_LOAD_MAX_SOROBAN_TX_COUNT *
                    config.SOROBAN_TRANSACTION_QUEUE_SIZE_MULTIPLIER +
                config.APPLY_LOAD_CLASSIC_TXS_PER_LEDGER + 2;
        }
        break;
    }
    if (config.APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS == 0)
    {
        throw std::runtime_error(
            "APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS cannot be zero");
    }
    setup();
}

void
ApplyLoad::setup()
{
    auto const& cfg = mApp.getConfig();
    if (cfg.GENESIS_TEST_ACCOUNT_COUNT < mNumAccounts)
    {
        throw std::runtime_error(
            "GENESIS_TEST_ACCOUNT_COUNT (" +
            std::to_string(cfg.GENESIS_TEST_ACCOUNT_COUNT) +
            ") must be at least " + std::to_string(mNumAccounts) +
            " for apply-load");
    }

    for (uint32_t i = 0; i < mNumAccounts; ++i)
    {
        auto acc =
            std::make_shared<TestAccount>(txtest::getGenesisAccount(mApp, i));
        releaseAssert(mTxGenerator.loadAccount(acc));
        mTxGenerator.addAccount(i, acc);
    }

    if (mApp.getLedgerManager()
            .getLastClosedLedgerHeader()
            .header.maxTxSetSize <
        mApp.getConfig().APPLY_LOAD_CLASSIC_TXS_PER_LEDGER)
    {
        auto upgrade = xdr::xvector<UpgradeType, 6>{};

        LedgerUpgrade ledgerUpgrade;
        ledgerUpgrade.type(LEDGER_UPGRADE_MAX_TX_SET_SIZE);
        ledgerUpgrade.newMaxTxSetSize() =
            mApp.getConfig().APPLY_LOAD_CLASSIC_TXS_PER_LEDGER;
        auto v = xdr::xdr_to_opaque(ledgerUpgrade);
        upgrade.push_back(UpgradeType{v.begin(), v.end()});
        closeLedger({}, upgrade);
    }

    setupUpgradeContract();

    // Set large resources for initial setup
    upgradeSettingsForMaxTPS(100000);

    // Make setup based on mode.
    switch (mMode)
    {
    case ApplyLoadMode::LIMIT_BASED:
    case ApplyLoadMode::FIND_LIMITS_FOR_MODEL_TX:
        setupLoadContract();
        break;
    case ApplyLoadMode::MAX_SAC_TPS:
        setupXLMContract();
        setupBatchTransferContracts();
        break;
    case ApplyLoadMode::BENCHMARK_MODEL_TX:
        switch (mModelTx)
        {
        case ApplyLoadModelTx::SAC:
            setupXLMContract();
            setupBatchTransferContracts();
            break;
        case ApplyLoadModelTx::CUSTOM_TOKEN:
            setupTokenContract();
            break;
        case ApplyLoadModelTx::SOROSWAP:
            setupSoroswapContracts();
            break;
        }
        break;
    }

    // Upgrade to final settings.
    switch (mMode)
    {
    case ApplyLoadMode::MAX_SAC_TPS:
    case ApplyLoadMode::FIND_LIMITS_FOR_MODEL_TX:
        // Just upgrade to a placeholder number of TXs, we'll
        // upgrade again before each TPS run.
        upgradeSettingsForMaxTPS(100000);
        break;
    case ApplyLoadMode::BENCHMARK_MODEL_TX:
        upgradeSettingsForMaxTPS(calculateBenchmarkModelTxCount());
        break;
    case ApplyLoadMode::LIMIT_BASED:
        upgradeSettings();
        break;
    }

    // Setup initial bucket list for modes that support it.
    if (mMode == ApplyLoadMode::LIMIT_BASED ||
        mMode == ApplyLoadMode::FIND_LIMITS_FOR_MODEL_TX)
    {
        setupBucketList();
    }
}

void
ApplyLoad::closeLedger(std::vector<TransactionFrameBasePtr> const& txs,
                       xdr::xvector<UpgradeType, 6> const& upgrades,
                       bool recordSorobanUtilization,
                       TxSetBuildPhaseTimings* txSetBuildTimings)
{
    auto txSet =
        makeTxSetFromTransactions(txs, mApp, 0, 0, false, {},
                                  txSetBuildTimings);

    if (recordSorobanUtilization)
    {
        auto ledgerResources = mApp.getLedgerManager().maxLedgerResources(true);
        auto txSetResources =
            txSet.second->getPhases()
                .at(static_cast<size_t>(TxSetPhase::SOROBAN))
                .getTotalResources(mApp.getLedgerManager()
                                       .getLastClosedLedgerHeader()
                                       .header.ledgerVersion)
                .value();
        mTxCountUtilization.Update(
            txSetResources.getVal(Resource::Type::OPERATIONS) * 1.0 /
            ledgerResources.getVal(Resource::Type::OPERATIONS) * 100000.0);
        mInstructionUtilization.Update(
            txSetResources.getVal(Resource::Type::INSTRUCTIONS) * 1.0 /
            ledgerResources.getVal(Resource::Type::INSTRUCTIONS) * 100000.0);
        mTxSizeUtilization.Update(
            txSetResources.getVal(Resource::Type::TX_BYTE_SIZE) * 1.0 /
            ledgerResources.getVal(Resource::Type::TX_BYTE_SIZE) * 100000.0);
        mDiskReadByteUtilization.Update(
            txSetResources.getVal(Resource::Type::DISK_READ_BYTES) * 1.0 /
            ledgerResources.getVal(Resource::Type::DISK_READ_BYTES) * 100000.0);
        mWriteByteUtilization.Update(
            txSetResources.getVal(Resource::Type::WRITE_BYTES) * 1.0 /
            ledgerResources.getVal(Resource::Type::WRITE_BYTES) * 100000.0);
        mDiskReadEntryUtilization.Update(
            txSetResources.getVal(Resource::Type::READ_LEDGER_ENTRIES) * 1.0 /
            ledgerResources.getVal(Resource::Type::READ_LEDGER_ENTRIES) *
            100000.0);
        mWriteEntryUtilization.Update(
            txSetResources.getVal(Resource::Type::WRITE_LEDGER_ENTRIES) * 1.0 /
            ledgerResources.getVal(Resource::Type::WRITE_LEDGER_ENTRIES) *
            100000.0);
        CLOG_INFO(Perf, "generated tx set resources: {}/{}",
                  txSetResources.toString(), ledgerResources.toString());
    }
    auto sv =
        mApp.getHerder().makeStellarValue(txSet.first->getContentsHash(), 1,
                                          upgrades, mApp.getConfig().NODE_SEED);

    stellar::txtest::closeLedger(mApp, txs, /* strictOrder */ false, upgrades);
}

void
ApplyLoad::execute()
{
    logExecutionEnvironmentSnapshot(mApp.getConfig());

    switch (mMode)
    {
    case ApplyLoadMode::LIMIT_BASED:
        benchmarkLimits();
        break;
    case ApplyLoadMode::MAX_SAC_TPS:
        findMaxSacTps();
        break;
    case ApplyLoadMode::FIND_LIMITS_FOR_MODEL_TX:
        findMaxLimitsForModelTransaction();
        break;
    case ApplyLoadMode::BENCHMARK_MODEL_TX:
        benchmarkModelTx();
        break;
    }
}

void
ApplyLoad::setupUpgradeContract()
{
    auto wasm = rust_bridge::get_write_bytes();
    xdr::opaque_vec<> wasmBytes;
    wasmBytes.assign(wasm.data.begin(), wasm.data.end());

    LedgerKey contractCodeLedgerKey;
    contractCodeLedgerKey.type(CONTRACT_CODE);
    contractCodeLedgerKey.contractCode().hash = sha256(wasmBytes);

    mUpgradeCodeKey = contractCodeLedgerKey;

    SorobanResources uploadResources;
    uploadResources.instructions = 2'000'000;
    uploadResources.diskReadBytes = 0;
    uploadResources.writeBytes = wasmBytes.size() + 500;

    auto const& lm = mApp.getLedgerManager();
    auto uploadTx = mTxGenerator.createUploadWasmTransaction(
        lm.getLastClosedLedgerNum() + 1, 0, wasmBytes, contractCodeLedgerKey,
        std::nullopt, uploadResources);

    closeLedger({uploadTx.second});

    auto salt = sha256("upgrade contract salt preimage");

    auto createTx = mTxGenerator.createContractTransaction(
        lm.getLastClosedLedgerNum() + 1, 0, contractCodeLedgerKey,
        wasmBytes.size() + 160, salt, std::nullopt);
    closeLedger({createTx.second});

    mUpgradeInstanceKey =
        createTx.second->sorobanResources().footprint.readWrite.back();

    releaseAssert(mTxGenerator.getApplySorobanSuccess().count() == 2);
}

// To upgrade settings, just modify mUpgradeConfig and then call
// upgradeSettings()
void
ApplyLoad::applyConfigUpgrade(SorobanUpgradeConfig const& upgradeConfig)
{
    int64_t currApplySorobanSuccess =
        mTxGenerator.getApplySorobanSuccess().count();
    auto const& lm = mApp.getLedgerManager();
    auto upgradeBytes =
        mTxGenerator.getConfigUpgradeSetFromLoadConfig(upgradeConfig);

    SorobanResources resources;
    resources.instructions = 1'250'000;
    resources.diskReadBytes = 0;
    resources.writeBytes = 3'100;

    auto [_, invokeTx] = mTxGenerator.invokeSorobanCreateUpgradeTransaction(
        lm.getLastClosedLedgerNum() + 1, TxGenerator::ROOT_ACCOUNT_ID,
        upgradeBytes, mUpgradeCodeKey, mUpgradeInstanceKey, std::nullopt,
        resources);
    {
        LedgerSnapshot ls(mApp);
        auto diagnostics =
            DiagnosticEventManager::createForValidation(mApp.getConfig());
        auto validationRes = invokeTx->checkValid(mApp.getAppConnector(), ls, 0,
                                                  0, 0, diagnostics);
        if (!validationRes->isSuccess())
        {
            if (validationRes->getResultCode() == txSOROBAN_INVALID)
            {
                diagnostics.debugLogEvents();
            }
            CLOG_FATAL(Perf, "Created invalid upgrade settings transaction: {}",
                       validationRes->getResultCode());
            releaseAssert(validationRes->isSuccess());
        }
    }

    auto upgradeSetKey = mTxGenerator.getConfigUpgradeSetKey(
        upgradeConfig,
        mUpgradeInstanceKey.contractData().contract.contractId());

    auto upgrade = xdr::xvector<UpgradeType, 6>{};
    auto ledgerUpgrade = LedgerUpgrade{LEDGER_UPGRADE_CONFIG};
    ledgerUpgrade.newConfig() = upgradeSetKey;
    auto v = xdr::xdr_to_opaque(ledgerUpgrade);
    upgrade.push_back(UpgradeType{v.begin(), v.end()});

    closeLedger({invokeTx}, upgrade);

    releaseAssert(mTxGenerator.getApplySorobanSuccess().count() -
                      currApplySorobanSuccess ==
                  1);
}

std::pair<SorobanUpgradeConfig, uint64_t>
ApplyLoad::updateSettingsForTxCount(uint64_t txsPerLedger)
{
    // Round the configuration values down to be a multiple of the respective
    // step in order to get more readable configurations, and also to speeed
    // up the binary search significantly.
    uint64_t const INSTRUCTIONS_ROUNDING_STEP = 5'000'000;
    uint64_t const SIZE_ROUNDING_STEP = 500;
    uint64_t const ENTRIES_ROUNDING_STEP = 10;

    auto const& config = mApp.getConfig();
    uint64_t insns =
        roundDown(txsPerLedger * config.APPLY_LOAD_INSTRUCTIONS[0] /
                      config.APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS,
                  INSTRUCTIONS_ROUNDING_STEP);
    uint64_t txSize = roundDown(
        txsPerLedger * config.APPLY_LOAD_TX_SIZE_BYTES[0], SIZE_ROUNDING_STEP);

    uint64_t writeEntries =
        roundDown(txsPerLedger * config.APPLY_LOAD_NUM_RW_ENTRIES[0],
                  ENTRIES_ROUNDING_STEP);
    uint64_t writeBytes = roundDown(
        writeEntries * config.APPLY_LOAD_DATA_ENTRY_SIZE, SIZE_ROUNDING_STEP);

    uint64_t diskReadEntries =
        roundDown(txsPerLedger * config.APPLY_LOAD_NUM_DISK_READ_ENTRIES[0],
                  ENTRIES_ROUNDING_STEP);
    uint64_t diskReadBytes =
        roundDown(diskReadEntries * config.APPLY_LOAD_DATA_ENTRY_SIZE,
                  SIZE_ROUNDING_STEP);

    if (diskReadEntries == 0)
    {
        diskReadEntries =
            MinimumSorobanNetworkConfig::TX_MAX_READ_LEDGER_ENTRIES;
        diskReadBytes = MinimumSorobanNetworkConfig::TX_MAX_READ_BYTES;
    }

    uint64_t actualMaxTxs = txsPerLedger;
    actualMaxTxs =
        std::min(actualMaxTxs,
                 insns * config.APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS /
                     config.APPLY_LOAD_INSTRUCTIONS[0]);
    actualMaxTxs =
        std::min(actualMaxTxs, txSize / config.APPLY_LOAD_TX_SIZE_BYTES[0]);
    if (config.APPLY_LOAD_NUM_DISK_READ_ENTRIES[0] > 0)
    {
        actualMaxTxs = std::min(actualMaxTxs,
                                diskReadEntries /
                                    config.APPLY_LOAD_NUM_DISK_READ_ENTRIES[0]);
        actualMaxTxs = std::min(
            actualMaxTxs,
            diskReadBytes / (config.APPLY_LOAD_NUM_DISK_READ_ENTRIES[0] *
                             config.APPLY_LOAD_DATA_ENTRY_SIZE));
    }
    actualMaxTxs = std::min(actualMaxTxs,
                            writeEntries / config.APPLY_LOAD_NUM_RW_ENTRIES[0]);

    actualMaxTxs = std::min(actualMaxTxs,
                            writeBytes / (config.APPLY_LOAD_NUM_RW_ENTRIES[0] *
                                          config.APPLY_LOAD_DATA_ENTRY_SIZE));
    CLOG_INFO(Perf,
              "Resources after rounding for testing {} actual max txs per "
              "ledger: "
              "instructions {}, tx size {}, disk read entries {}, "
              "disk read bytes {}, rw entries {}, rw bytes {}",
              actualMaxTxs, insns, txSize, diskReadEntries, diskReadBytes,
              writeEntries, writeBytes);

    auto upgradeConfig = getUpgradeConfig(mApp.getConfig(),
                                          /* validate */ false);
    // Set tx limits to the respective resources of the 'model'
    // transaction.
    upgradeConfig.txMaxInstructions =
        std::max(MinimumSorobanNetworkConfig::TX_MAX_INSTRUCTIONS,
                 config.APPLY_LOAD_INSTRUCTIONS[0]);
    upgradeConfig.txMaxSizeBytes =
        std::max(MinimumSorobanNetworkConfig::TX_MAX_SIZE_BYTES,
                 config.APPLY_LOAD_TX_SIZE_BYTES[0]);
    upgradeConfig.txMaxDiskReadEntries =
        std::max(MinimumSorobanNetworkConfig::TX_MAX_READ_LEDGER_ENTRIES,
                 config.APPLY_LOAD_NUM_DISK_READ_ENTRIES[0]);
    upgradeConfig.txMaxWriteLedgerEntries =
        std::max(MinimumSorobanNetworkConfig::TX_MAX_WRITE_LEDGER_ENTRIES,
                 config.APPLY_LOAD_NUM_RW_ENTRIES[0]);
    upgradeConfig.txMaxDiskReadBytes =
        std::max(MinimumSorobanNetworkConfig::TX_MAX_READ_BYTES,
                 config.APPLY_LOAD_NUM_DISK_READ_ENTRIES[0] *
                     config.APPLY_LOAD_DATA_ENTRY_SIZE);
    upgradeConfig.txMaxWriteBytes =
        std::max(MinimumSorobanNetworkConfig::TX_MAX_WRITE_BYTES,
                 config.APPLY_LOAD_NUM_RW_ENTRIES[0] *
                     config.APPLY_LOAD_DATA_ENTRY_SIZE);
    upgradeConfig.txMaxContractEventsSizeBytes =
        std::max(MinimumSorobanNetworkConfig::TX_MAX_CONTRACT_EVENTS_SIZE_BYTES,
                 config.APPLY_LOAD_EVENT_COUNT[0] *
                         TxGenerator::SOROBAN_LOAD_V2_EVENT_SIZE_BYTES +
                     100);
    upgradeConfig.txMaxFootprintEntries =
        *upgradeConfig.txMaxDiskReadEntries +
        *upgradeConfig.txMaxWriteLedgerEntries;

    // Set the ledger-wide limits to the compute values calculated above.
    // Note, that in theory we could end up with ledger limits lower than
    // the transaction limits, but in normally would be just
    // mis-configuration (using a model transaction that is too large to
    // be applied within the target close time).
    upgradeConfig.ledgerMaxInstructions = insns;
    upgradeConfig.ledgerMaxTransactionsSizeBytes = txSize;
    upgradeConfig.ledgerMaxDiskReadEntries = diskReadEntries;
    upgradeConfig.ledgerMaxWriteLedgerEntries = writeEntries;
    upgradeConfig.ledgerMaxDiskReadBytes = diskReadBytes;
    upgradeConfig.ledgerMaxWriteBytes = writeBytes;

    return std::make_pair(upgradeConfig, actualMaxTxs);
}

void
ApplyLoad::upgradeSettings()
{
    releaseAssertOrThrow(mMode != ApplyLoadMode::MAX_SAC_TPS);

    auto upgradeConfig = getUpgradeConfig(mApp.getConfig());
    applyConfigUpgrade(upgradeConfig);
}

void
ApplyLoad::setupLoadContract()
{
    auto wasm = rust_bridge::get_test_wasm_loadgen();
    xdr::opaque_vec<> wasmBytes;
    wasmBytes.assign(wasm.data.begin(), wasm.data.end());

    LedgerKey contractCodeLedgerKey;
    contractCodeLedgerKey.type(CONTRACT_CODE);
    contractCodeLedgerKey.contractCode().hash = sha256(wasmBytes);

    mLoadCodeKey = contractCodeLedgerKey;

    int64_t currApplySorobanSuccess =
        mTxGenerator.getApplySorobanSuccess().count();

    auto const& lm = mApp.getLedgerManager();
    auto uploadTx = mTxGenerator.createUploadWasmTransaction(
        lm.getLastClosedLedgerNum() + 1, 0, wasmBytes, contractCodeLedgerKey,
        std::nullopt);

    closeLedger({uploadTx.second});

    auto salt = sha256("Load contract");

    auto createTx = mTxGenerator.createContractTransaction(
        lm.getLastClosedLedgerNum() + 1, 0, contractCodeLedgerKey,
        wasmBytes.size() + 160, salt, std::nullopt);
    closeLedger({createTx.second});

    releaseAssert(mTxGenerator.getApplySorobanSuccess().count() -
                      currApplySorobanSuccess ==
                  2);
    releaseAssert(mTxGenerator.getApplySorobanFailure().count() == 0);

    auto instanceKey =
        createTx.second->sorobanResources().footprint.readWrite.back();

    mLoadInstance.readOnlyKeys.emplace_back(mLoadCodeKey);
    mLoadInstance.readOnlyKeys.emplace_back(instanceKey);
    mLoadInstance.contractID = instanceKey.contractData().contract;
    mLoadInstance.contractEntriesSize =
        footprintSize(mApp, mLoadInstance.readOnlyKeys);
}

void
ApplyLoad::setupXLMContract()
{
    int64_t currApplySorobanSuccess =
        mTxGenerator.getApplySorobanSuccess().count();

    auto createTx = mTxGenerator.createSACTransaction(
        mApp.getLedgerManager().getLastClosedLedgerNum() + 1, 0,
        txtest::makeNativeAsset(), std::nullopt);
    closeLedger({createTx.second});

    releaseAssert(mTxGenerator.getApplySorobanSuccess().count() -
                      currApplySorobanSuccess ==
                  1);
    releaseAssert(mTxGenerator.getApplySorobanFailure().count() == 0);

    auto instanceKey =
        createTx.second->sorobanResources().footprint.readWrite.back();

    mSACInstanceXLM.readOnlyKeys.emplace_back(instanceKey);
    mSACInstanceXLM.contractID = instanceKey.contractData().contract;
    mSACInstanceXLM.contractEntriesSize =
        footprintSize(mApp, mSACInstanceXLM.readOnlyKeys);
}

void
ApplyLoad::setupBatchTransferContracts()
{
    auto const& lm = mApp.getLedgerManager();

    // First, upload the batch_transfer contract Wasm
    auto wasm = rust_bridge::get_test_contract_sac_transfer(
        mApp.getConfig().LEDGER_PROTOCOL_VERSION);
    xdr::opaque_vec<> wasmBytes;
    wasmBytes.assign(wasm.data.begin(), wasm.data.end());

    LedgerKey contractCodeLedgerKey;
    contractCodeLedgerKey.type(CONTRACT_CODE);
    contractCodeLedgerKey.contractCode().hash = sha256(wasmBytes);

    SorobanResources uploadResources;
    uploadResources.instructions = 5000000;
    uploadResources.diskReadBytes = 0;
    uploadResources.writeBytes = wasmBytes.size() + 500;

    auto uploadTx = mTxGenerator.createUploadWasmTransaction(
        lm.getLastClosedLedgerNum() + 1, TxGenerator::ROOT_ACCOUNT_ID,
        wasmBytes, contractCodeLedgerKey, std::nullopt, uploadResources);
    closeLedger({uploadTx.second});

    // Since we transfer from the batch transfer contract balance, deploy one
    // contract for each cluster to maximize parallelism
    uint32_t numClusters =
        mApp.getConfig().APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS;
    mBatchTransferInstances.reserve(numClusters);

    for (uint32_t i = 0; i < numClusters; ++i)
    {
        auto successCountBefore = mTxGenerator.getApplySorobanSuccess().count();
        auto salt = sha256(std::to_string(i));

        auto createTx = mTxGenerator.createContractTransaction(
            lm.getLastClosedLedgerNum() + 1, TxGenerator::ROOT_ACCOUNT_ID,
            contractCodeLedgerKey, wasmBytes.size() + 160, salt, std::nullopt);
        closeLedger({createTx.second});

        auto instanceKey =
            createTx.second->sorobanResources().footprint.readWrite.back();

        TxGenerator::ContractInstance instance;
        instance.readOnlyKeys.emplace_back(contractCodeLedgerKey);
        instance.readOnlyKeys.emplace_back(instanceKey);
        instance.contractID = instanceKey.contractData().contract;
        instance.contractEntriesSize =
            footprintSize(mApp, instance.readOnlyKeys);

        mBatchTransferInstances.push_back(instance);

        // Initialize XLM balance for the batch_transfer contract
        // We need to transfer enough XLM to cover all batch transfers
        // Each batch will transfer APPLY_LOAD_BATCH_SAC_COUNT * 1 stroop
        int64_t maxTxsPerCluster =
            mApp.getConfig().APPLY_LOAD_MAX_SAC_TPS_MAX_TPS / numClusters;
        int64_t amountToTransfer =
            mApp.getConfig().APPLY_LOAD_BATCH_SAC_COUNT * // Sent per tx
            maxTxsPerCluster * // Max txs per ledger per cluster
            mApp.getConfig().APPLY_LOAD_NUM_LEDGERS * // Number of ledgers
            10;                                       // Buffer

        auto transferTx = mTxGenerator.invokeSACPayment(
            lm.getLastClosedLedgerNum() + 1, TxGenerator::ROOT_ACCOUNT_ID,
            instance.contractID, mSACInstanceXLM, amountToTransfer, 10'000'000);
        closeLedger({transferTx.second});

        auto successCountAfter = mTxGenerator.getApplySorobanSuccess().count();

        // Verify both instantiate and fund transactions succeeded
        releaseAssertOrThrow(successCountAfter == successCountBefore + 2);
        releaseAssertOrThrow(mTxGenerator.getApplySorobanFailure().count() ==
                             0);
    }

    releaseAssertOrThrow(mBatchTransferInstances.size() == numClusters);
}

void
ApplyLoad::setupBucketList()
{
    auto lh = mApp.getLedgerManager().getLastClosedLedgerHeader().header;
    auto& bl = mApp.getBucketManager().getLiveBucketList();
    auto& hotArchiveBl = mApp.getBucketManager().getHotArchiveBucketList();
    auto const& cfg = mApp.getConfig();

    uint64_t currentLiveKey = 0;
    uint64_t currentHotArchiveKey = 0;

    // Prepare base entries for both live and hot archive
    LedgerEntry baseLiveEntry;
    baseLiveEntry.data.type(CONTRACT_DATA);
    baseLiveEntry.data.contractData().contract = mLoadInstance.contractID;
    baseLiveEntry.data.contractData().key.type(SCV_U64);
    baseLiveEntry.data.contractData().key.u64() = 0;
    baseLiveEntry.data.contractData().durability =
        ContractDataDurability::PERSISTENT;
    baseLiveEntry.data.contractData().val.type(SCV_BYTES);

    mDataEntrySize = xdr::xdr_size(baseLiveEntry);
    // Add some padding to reach the configured LE size.
    if (mDataEntrySize < mApp.getConfig().APPLY_LOAD_DATA_ENTRY_SIZE)
    {
        baseLiveEntry.data.contractData().val.bytes().resize(
            mApp.getConfig().APPLY_LOAD_DATA_ENTRY_SIZE - mDataEntrySize);
        mDataEntrySize = mApp.getConfig().APPLY_LOAD_DATA_ENTRY_SIZE;
        releaseAssertOrThrow(xdr::xdr_size(baseLiveEntry) == mDataEntrySize);
    }
    else
    {
        CLOG_WARNING(Perf,
                     "Apply load generated entry size is larger than "
                     "APPLY_LOAD_DATA_ENTRY_SIZE: {} > {}",
                     mApp.getConfig().APPLY_LOAD_DATA_ENTRY_SIZE,
                     mDataEntrySize);
    }

    auto logBucketListStats = [](std::string const& logStr,
                                 auto const& bucketList) {
        CLOG_INFO(Bucket, "{}", logStr);
        for (uint32_t i = 0;
             i < std::remove_reference_t<decltype(bucketList)>::kNumLevels; ++i)
        {
            auto const& lev = bucketList.getLevel(i);
            auto currSz = BucketTestUtils::countEntries(lev.getCurr());
            auto snapSz = BucketTestUtils::countEntries(lev.getSnap());
            CLOG_INFO(Bucket, "Level {}: {} = {} + {}", i, currSz + snapSz,
                      currSz, snapSz);
        }
    };

    LedgerEntry baseHotArchiveEntry = baseLiveEntry;

    // Hot archive entries are added every APPLY_LOAD_BL_WRITE_FREQUENCY
    // ledgers, but save one batch for the last batch to populate upper levels.
    uint32_t totalBatchCount =
        cfg.APPLY_LOAD_BL_SIMULATED_LEDGERS / cfg.APPLY_LOAD_BL_WRITE_FREQUENCY;
    releaseAssertOrThrow(totalBatchCount > 0);

    // Reserve one batch worth of entries for the top level buckets.
    uint32_t hotArchiveBatchCount = totalBatchCount - 1;
    uint32_t hotArchiveBatchSize =
        mTotalHotArchiveEntries / (totalBatchCount + 1);

    // To populate the first few levels of the hot archive BL, we write the
    // remaining entries over APPLY_LOAD_BL_LAST_BATCH_LEDGERS ledgers.
    uint32_t hotArchiveLastBatchSize =
        mTotalHotArchiveEntries > 0
            ? ceil(static_cast<double>(
                       mTotalHotArchiveEntries -
                       (hotArchiveBatchSize * hotArchiveBatchCount)) /
                   cfg.APPLY_LOAD_BL_LAST_BATCH_LEDGERS)
            : 0;

    CLOG_INFO(Perf,
              "Apply load: Hot Archive BL setup: total entries {}, total "
              "batches {}, batch size {}, last batch size {}",
              mTotalHotArchiveEntries, totalBatchCount, hotArchiveBatchSize,
              hotArchiveLastBatchSize);

    for (uint32_t i = 0; i < cfg.APPLY_LOAD_BL_SIMULATED_LEDGERS; ++i)
    {
        if (i % 1000 == 0)
        {
            logBucketListStats(
                fmt::format("Generating BL ledger {}, levels thus far", i), bl);

            if (mTotalHotArchiveEntries > 0)
            {
                logBucketListStats(
                    fmt::format(
                        "Generating hot archive BL ledger {}, levels thus far",
                        i),
                    hotArchiveBl);
            }
        }
        lh.ledgerSeq++;

        std::vector<LedgerEntry> liveEntries;
        std::vector<LedgerEntry> archivedEntries;
        bool isLastBatch = i >= cfg.APPLY_LOAD_BL_SIMULATED_LEDGERS -
                                    cfg.APPLY_LOAD_BL_LAST_BATCH_LEDGERS;
        if (i % cfg.APPLY_LOAD_BL_WRITE_FREQUENCY == 0 || isLastBatch)
        {
            uint32_t entryCount = isLastBatch
                                      ? cfg.APPLY_LOAD_BL_LAST_BATCH_SIZE
                                      : cfg.APPLY_LOAD_BL_BATCH_SIZE;
            for (uint32_t j = 0; j < entryCount; j++)
            {
                LedgerEntry le = baseLiveEntry;
                le.lastModifiedLedgerSeq = lh.ledgerSeq;
                le.data.contractData().key.u64() = currentLiveKey++;
                liveEntries.push_back(le);

                LedgerEntry ttlEntry;
                ttlEntry.data.type(TTL);
                ttlEntry.lastModifiedLedgerSeq = lh.ledgerSeq;
                ttlEntry.data.ttl().keyHash = xdrSha256(LedgerEntryKey(le));
                ttlEntry.data.ttl().liveUntilLedgerSeq = 1'000'000'000;
                liveEntries.push_back(ttlEntry);
            }

            uint32_t archivedEntryCount =
                isLastBatch ? hotArchiveLastBatchSize : hotArchiveBatchSize;
            for (uint32_t j = 0; j < archivedEntryCount; j++)
            {
                LedgerEntry le = baseHotArchiveEntry;
                le.lastModifiedLedgerSeq = lh.ledgerSeq;

                auto lk = getKeyForArchivedEntry(currentHotArchiveKey);
                le.data.contractData().contract = lk.contractData().contract;
                le.data.contractData().key = lk.contractData().key;
                le.data.contractData().durability =
                    lk.contractData().durability;
                le.data.contractData().val =
                    baseLiveEntry.data.contractData().val;

                archivedEntries.push_back(le);
                ++currentHotArchiveKey;
            }
        }

        bl.addBatch(mApp, lh.ledgerSeq, lh.ledgerVersion, liveEntries, {}, {});
        if (mTotalHotArchiveEntries > 0)
        {
            hotArchiveBl.addBatch(mApp, lh.ledgerSeq, lh.ledgerVersion,
                                  archivedEntries, {});
        }
    }
    mDataEntryCount = currentLiveKey;
    releaseAssertOrThrow(mTotalHotArchiveEntries <= currentHotArchiveKey);

    logBucketListStats("Final generated live bucket list levels", bl);
    if (mTotalHotArchiveEntries > 0)
    {
        logBucketListStats("Final generated hot archive bucket list levels",
                           hotArchiveBl);
    }

    HistoryArchiveState has;
    has.currentLedger = lh.ledgerSeq;
    mApp.getPersistentState().setMainState(
        PersistentState::kHistoryArchiveState, has.toString(),
        mApp.getDatabase().getSession());
    mApp.getBucketManager().snapshotLedger(lh);
    {
        LedgerTxn ltx(mApp.getLedgerTxnRoot());
        ltx.loadHeader().current() = lh;
        mApp.getLedgerManager().manuallyAdvanceLedgerHeader(
            ltx.loadHeader().current());
        ltx.commit();
    }
    mApp.getLedgerManager().storeCurrentLedgerForTest(lh);
    mApp.getLedgerManager().rebuildInMemorySorobanStateForTesting(
        lh.ledgerVersion);
    mApp.getHerder().forceSCPStateIntoSyncWithLastClosedLedger();
    closeLedger({}, {});
}

void
ApplyLoad::benchmarkLimits()
{
    auto& ledgerClose =
        mApp.getMetrics().NewTimer({"ledger", "ledger", "close"});
    ledgerClose.Clear();

    auto& cpuInsRatio = mApp.getMetrics().NewHistogram(
        {"soroban", "host-fn-op", "invoke-time-fsecs-cpu-insn-ratio"});
    cpuInsRatio.Clear();

    auto& cpuInsRatioExclVm = mApp.getMetrics().NewHistogram(
        {"soroban", "host-fn-op", "invoke-time-fsecs-cpu-insn-ratio-excl-vm"});
    cpuInsRatioExclVm.Clear();

    auto& ledgerCpuInsRatio = mApp.getMetrics().NewHistogram(
        {"soroban", "host-fn-op", "ledger-cpu-insns-ratio"});
    ledgerCpuInsRatio.Clear();

    auto& ledgerCpuInsRatioExclVm = mApp.getMetrics().NewHistogram(
        {"soroban", "host-fn-op", "ledger-cpu-insns-ratio-excl-vm"});
    ledgerCpuInsRatioExclVm.Clear();

    auto& totalTxApplyTime =
        mApp.getMetrics().NewTimer({"ledger", "transaction", "total-apply"});
    totalTxApplyTime.Clear();

    for (size_t i = 0; i < mApp.getConfig().APPLY_LOAD_NUM_LEDGERS; ++i)
    {
        benchmarkLimitsIteration();
    }
    CLOG_INFO(Perf,
              "Ledger close min/avg/max: {}/{}/{} milliseconds "
              "(stddev={})",
              ledgerClose.min(), ledgerClose.mean(), ledgerClose.max(),
              ledgerClose.std_dev());
    CLOG_INFO(Perf,
              "Tx apply time min/avg/max: {}/{}/{} milliseconds "
              "(stddev={})",
              totalTxApplyTime.min(), totalTxApplyTime.mean(),
              totalTxApplyTime.max(), totalTxApplyTime.std_dev());

    CLOG_INFO(Perf, "Max CPU ins ratio: {}", cpuInsRatio.max() / 1000000);
    CLOG_INFO(Perf, "Mean CPU ins ratio:  {}", cpuInsRatio.mean() / 1000000);

    CLOG_INFO(Perf, "Max CPU ins ratio excl VM: {}",
              cpuInsRatioExclVm.max() / 1000000);
    CLOG_INFO(Perf, "Mean CPU ins ratio excl VM:  {}",
              cpuInsRatioExclVm.mean() / 1000000);
    CLOG_INFO(Perf, "stddev CPU ins ratio excl VM:  {}",
              cpuInsRatioExclVm.std_dev() / 1000000);

    CLOG_INFO(Perf, "Ledger Max CPU ins ratio: {}",
              ledgerCpuInsRatio.max() / 1000000);
    CLOG_INFO(Perf, "Ledger Mean CPU ins ratio:  {}",
              ledgerCpuInsRatio.mean() / 1000000);
    CLOG_INFO(Perf, "Ledger stddev CPU ins ratio:  {}",
              ledgerCpuInsRatio.std_dev() / 1000000);

    CLOG_INFO(Perf, "Ledger Max CPU ins ratio excl VM: {}",
              ledgerCpuInsRatioExclVm.max() / 1000000);
    CLOG_INFO(Perf, "Ledger Mean CPU ins ratio excl VM:  {}",
              ledgerCpuInsRatioExclVm.mean() / 1000000);
    CLOG_INFO(Perf, "Ledger stddev CPU ins ratio excl VM:  {} milliseconds",
              ledgerCpuInsRatioExclVm.std_dev() / 1000000);
    CLOG_INFO(Perf, "Tx count utilization min/avg/max {}/{}/{}%",
              getTxCountUtilization().min() / 1000.0,
              getTxCountUtilization().mean() / 1000.0,
              getTxCountUtilization().max() / 1000.0);
    CLOG_INFO(Perf, "Instruction utilization min/avg/max {}/{}/{}%",
              getInstructionUtilization().min() / 1000.0,
              getInstructionUtilization().mean() / 1000.0,
              getInstructionUtilization().max() / 1000.0);
    CLOG_INFO(Perf, "Tx size utilization min/avg/max {}/{}/{}%",
              getTxSizeUtilization().min() / 1000.0,
              getTxSizeUtilization().mean() / 1000.0,
              getTxSizeUtilization().max() / 1000.0);
    CLOG_INFO(Perf, "Disk read bytes utilization min/avg/max {}/{}/{}%",
              getDiskReadByteUtilization().min() / 1000.0,
              getDiskReadByteUtilization().mean() / 1000.0,
              getDiskReadByteUtilization().max() / 1000.0);
    CLOG_INFO(Perf, "Write bytes utilization min/avg/max {}/{}/{}%",
              getDiskWriteByteUtilization().min() / 1000.0,
              getDiskWriteByteUtilization().mean() / 1000.0,
              getDiskWriteByteUtilization().max() / 1000.0);
    CLOG_INFO(Perf, "Disk read entry utilization min/avg/max {}/{}/{}%",
              getDiskReadEntryUtilization().min() / 1000.0,
              getDiskReadEntryUtilization().mean() / 1000.0,
              getDiskReadEntryUtilization().max() / 1000.0);
    CLOG_INFO(Perf, "Write entry utilization min/avg/max {}/{}/{}%",
              getWriteEntryUtilization().min() / 1000.0,
              getWriteEntryUtilization().mean() / 1000.0,
              getWriteEntryUtilization().max() / 1000.0);

    CLOG_INFO(Perf, "Tx Success Rate: {:f}%", successRate() * 100);
}

double
ApplyLoad::benchmarkLimitsIteration()
{
    mApp.getBucketManager().getLiveBucketList().resolveAllFutures();
    releaseAssert(
        mApp.getBucketManager().getLiveBucketList().futuresAllResolved());
    mApp.getBucketManager().getHotArchiveBucketList().resolveAllFutures();
    releaseAssert(
        mApp.getBucketManager().getHotArchiveBucketList().futuresAllResolved());

    auto& lm = mApp.getLedgerManager();
    auto const& config = mApp.getConfig();
    std::vector<TransactionFrameBasePtr> txs;

    auto maxResourcesToGenerate = lm.maxLedgerResources(true);
    // The TxSet validation will compare the ledger instruction limit
    // against the sum of the instructions of the slowest cluster in each
    // stage, so we just multiply the instructions limit by the max number
    // of clusters.
    maxResourcesToGenerate.setVal(
        Resource::Type::INSTRUCTIONS,
        maxResourcesToGenerate.getVal(Resource::Type::INSTRUCTIONS) *
            config.APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS);
    // Scale the resources by the tx queue multipler to emulate filled
    // mempool.
    maxResourcesToGenerate =
        multiplyByDouble(maxResourcesToGenerate,
                         config.SOROBAN_TRANSACTION_QUEUE_SIZE_MULTIPLIER);

    CLOG_INFO(Perf, "benchmark max generation resources: {}",
              maxResourcesToGenerate.toString());
    auto resourcesLeft = maxResourcesToGenerate;

    // Generate classic payments using the first
    // APPLY_LOAD_CLASSIC_TXS_PER_LEDGER accounts.
    generateClassicPayments(txs, 0);

    // Use remaining accounts (after classic) for soroban transactions
    auto const& accounts = mTxGenerator.getAccounts();
    uint32_t sorobanStartIdx = config.APPLY_LOAD_CLASSIC_TXS_PER_LEDGER;
    // Omit root account
    std::vector<uint64_t> shuffledAccounts(accounts.size() - 1 -
                                           sorobanStartIdx);
    std::iota(shuffledAccounts.begin(), shuffledAccounts.end(),
              sorobanStartIdx);
    stellar::shuffle(std::begin(shuffledAccounts), std::end(shuffledAccounts),
                     getGlobalRandomEngine());

    LedgerSnapshot ls(mApp);
    auto appConnector = mApp.getAppConnector();

    auto addTx = [&ls, &appConnector, &txs](TransactionFrameBasePtr tx) {
        auto diagnostics = DiagnosticEventManager::createDisabled();
        auto res = tx->checkValid(appConnector, ls, 0, 0, 0, diagnostics);
        releaseAssert(res && res->isSuccess());
        txs.emplace_back(tx);
    };

    bool sorobanLimitHit = false;
    for (size_t i = 0; i < shuffledAccounts.size(); ++i)
    {
        auto it = accounts.find(shuffledAccounts[i]);
        releaseAssert(it != accounts.end());

        auto [_, tx] = mTxGenerator.invokeSorobanLoadTransactionV2(
            lm.getLastClosedLedgerNum() + 1, it->first, mLoadInstance,
            mDataEntryCount, mDataEntrySize, 1'000'000);

        uint32_t ledgerVersion = mApp.getLedgerManager()
                                     .getLastClosedLedgerHeader()
                                     .header.ledgerVersion;
        auto txResources = tx->getResources(false, ledgerVersion);
        if (!anyGreater(txResources, resourcesLeft))
        {
            resourcesLeft -= txResources;
        }
        else
        {
            for (size_t i = 0; i < resourcesLeft.size(); ++i)
            {
                auto type = static_cast<Resource::Type>(i);
                if (txResources.getVal(type) > resourcesLeft.getVal(type))
                {
                    auto resourcesGenerated = maxResourcesToGenerate;
                    resourcesGenerated -= resourcesLeft;
                    CLOG_INFO(Perf,
                              "Ledger {} limit hit during tx generation, "
                              "total resources generated: {}, not fitting tx "
                              "resources: {}",
                              Resource::getStringFromType(type),
                              resourcesGenerated.toString(),
                              txResources.toString());
                    sorobanLimitHit = true;
                }
            }

            break;
        }
        addTx(tx);
    }
    // If this assert fails, it most likely means that we ran out of
    // accounts, which should not happen.
    releaseAssert(sorobanLimitHit);

    auto& ledgerCloseTime =
        mApp.getMetrics().NewTimer({"ledger", "ledger", "close"});

    double timeBefore = ledgerCloseTime.sum();
    closeLedger(txs, {}, /* recordSorobanUtilization */ true);
    double timeAfter = ledgerCloseTime.sum();

    double closeTime = timeAfter - timeBefore;
    CLOG_INFO(Perf, "Limits benchmark time: {:.2f}ms", closeTime);
    return closeTime;
}

void
ApplyLoad::findMaxLimitsForModelTransaction()
{
    auto const& config = mApp.getConfig();

    auto validateTxParam = [&config](std::string const& paramName,
                                     auto const& values, auto const& weights,
                                     bool allowZeroValue = false) {
        if (values.size() != 1)
        {
            throw std::runtime_error(
                fmt::format(FMT_STRING("{} must have exactly one entry for "
                                       "'limits-for-model-tx' mode"),
                            paramName));
        }
        if (!allowZeroValue && values[0] == 0)
        {
            throw std::runtime_error(fmt::format(
                FMT_STRING("{} cannot be zero for 'limits-for-model-tx' mode"),
                paramName));
        }
        if (weights.size() != 1 || weights[0] != 1)
        {
            throw std::runtime_error(
                fmt::format(FMT_STRING("{}_DISTRIBUTION must have exactly one "
                                       "entry with the value of 1 for "
                                       "'limits-for-model-tx' mode"),
                            paramName));
        }
    };
    validateTxParam("APPLY_LOAD_INSTRUCTIONS", config.APPLY_LOAD_INSTRUCTIONS,
                    config.APPLY_LOAD_INSTRUCTIONS_DISTRIBUTION);
    validateTxParam("APPLY_LOAD_TX_SIZE_BYTES", config.APPLY_LOAD_TX_SIZE_BYTES,
                    config.APPLY_LOAD_TX_SIZE_BYTES_DISTRIBUTION);
    validateTxParam("APPLY_LOAD_NUM_DISK_READ_ENTRIES",
                    config.APPLY_LOAD_NUM_DISK_READ_ENTRIES,
                    config.APPLY_LOAD_NUM_DISK_READ_ENTRIES_DISTRIBUTION, true);
    validateTxParam("APPLY_LOAD_NUM_RW_ENTRIES",
                    config.APPLY_LOAD_NUM_RW_ENTRIES,
                    config.APPLY_LOAD_NUM_RW_ENTRIES_DISTRIBUTION);
    validateTxParam("APPLY_LOAD_EVENT_COUNT", config.APPLY_LOAD_EVENT_COUNT,
                    config.APPLY_LOAD_EVENT_COUNT_DISTRIBUTION, true);

    double targetTimeMs = mApp.getConfig().APPLY_LOAD_TARGET_CLOSE_TIME_MS;

    // Track the best config found during the search
    SorobanUpgradeConfig maxLimitsConfig;
    uint64_t maxLimitsTxsPerLedger = 0;

    auto prepareIteration = [this, &config](uint32_t testTxsPerLedger) {
        CLOG_INFO(Perf,
                  "Testing ledger max model txs: {}, generated limits: "
                  "instructions {}, tx size {}, disk read entries {}, rw "
                  "entries {}",
                  testTxsPerLedger,
                  testTxsPerLedger * config.APPLY_LOAD_INSTRUCTIONS[0],
                  testTxsPerLedger * config.APPLY_LOAD_TX_SIZE_BYTES[0],
                  testTxsPerLedger * config.APPLY_LOAD_NUM_DISK_READ_ENTRIES[0],
                  testTxsPerLedger * config.APPLY_LOAD_NUM_RW_ENTRIES[0]);

        auto [upgradeConfig, actualMaxTxsPerLedger] =
            updateSettingsForTxCount(testTxsPerLedger);

        applyConfigUpgrade(upgradeConfig);
    };
    auto iterationResult = [this, &maxLimitsTxsPerLedger, &maxLimitsConfig](
                               uint32_t testTxsPerLedger, bool isAbove) {
        auto [upgradeConfig, actualMaxTxsPerLedger] =
            updateSettingsForTxCount(testTxsPerLedger);
        // Store the config if this is the best so far
        if (!isAbove && actualMaxTxsPerLedger > maxLimitsTxsPerLedger)
        {
            maxLimitsTxsPerLedger = actualMaxTxsPerLedger;
            maxLimitsConfig = upgradeConfig;
        }
    };

    auto benchmarkFunc = [this](uint32_t testTxsPerLedger) -> double {
        double closeTime = benchmarkLimitsIteration();
        releaseAssert(successRate() == 1.0);
        return closeTime;
    };

    uint32_t minTxsPerLedger = 1;
    uint32_t maxTxsPerLedger = mApp.getConfig().APPLY_LOAD_MAX_SOROBAN_TX_COUNT;
    size_t maxSamplesPerPoint = mApp.getConfig().APPLY_LOAD_NUM_LEDGERS;
    uint32_t xTolerance = 100;

    auto [lo, hi] = noisyBinarySearch(
        benchmarkFunc, targetTimeMs, minTxsPerLedger, maxTxsPerLedger,
        NOISY_BINARY_SEARCH_CONFIDENCE, xTolerance, maxSamplesPerPoint,
        prepareIteration, iterationResult);
    // Note, that the final search range may be above the TPL found, that's due
    // to rounding we do when calculating TPL to benchmark (not every TPL
    // value can be tested fairly).
    CLOG_INFO(Perf,
              "Maximum limits found for model transaction ({} TPL, [{}, {}] "
              "final search range): "
              "instructions {}, "
              "tx size {}, disk read entries {}, disk read bytes {}, "
              "write entries {}, write bytes {}",
              maxLimitsTxsPerLedger, lo, hi,
              *maxLimitsConfig.ledgerMaxInstructions,
              *maxLimitsConfig.ledgerMaxTransactionsSizeBytes,
              *maxLimitsConfig.ledgerMaxDiskReadEntries,
              *maxLimitsConfig.ledgerMaxDiskReadBytes,
              *maxLimitsConfig.ledgerMaxWriteLedgerEntries,
              *maxLimitsConfig.ledgerMaxWriteBytes);
}

double
ApplyLoad::successRate()
{
    auto& success =
        mApp.getMetrics().NewCounter({"ledger", "apply", "success"});
    auto& failure =
        mApp.getMetrics().NewCounter({"ledger", "apply", "failure"});
    return success.count() * 1.0 / (success.count() + failure.count());
}

medida::Histogram const&
ApplyLoad::getTxCountUtilization()
{
    return mTxCountUtilization;
}
medida::Histogram const&
ApplyLoad::getInstructionUtilization()
{
    return mInstructionUtilization;
}
medida::Histogram const&
ApplyLoad::getTxSizeUtilization()
{
    return mTxSizeUtilization;
}
medida::Histogram const&
ApplyLoad::getDiskReadByteUtilization()
{
    return mDiskReadByteUtilization;
}
medida::Histogram const&
ApplyLoad::getDiskWriteByteUtilization()
{
    return mWriteByteUtilization;
}
medida::Histogram const&
ApplyLoad::getDiskReadEntryUtilization()
{
    return mDiskReadEntryUtilization;
}
medida::Histogram const&
ApplyLoad::getWriteEntryUtilization()
{
    return mWriteEntryUtilization;
}

void
ApplyLoad::warmAccountCache()
{
    auto const& accounts = mTxGenerator.getAccounts();
    CLOG_INFO(Perf, "Warming account cache with {} accounts.", accounts.size());

    LedgerTxn ltx(mApp.getLedgerTxnRoot());
    for (auto const& [_, account] : accounts)
    {
        auto acc = stellar::loadAccount(ltx, account->getPublicKey());
        releaseAssert(acc);
    }
}

void
ApplyLoad::findMaxSacTps()
{
    uint32_t const MIN_TXS_PER_STEP = 64;
    releaseAssertOrThrow(mMode == ApplyLoadMode::MAX_SAC_TPS);

    uint32_t numClusters =
        mApp.getConfig().APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS;
    uint32_t txsPerStep =
        numClusters * mApp.getConfig().APPLY_LOAD_BATCH_SAC_COUNT;
    if (txsPerStep < MIN_TXS_PER_STEP)
    {
        txsPerStep =
            std::ceil(static_cast<double>(MIN_TXS_PER_STEP) / txsPerStep) *
            txsPerStep;
    }
    uint32_t minSteps = std::max(
        1u, mApp.getConfig().APPLY_LOAD_MAX_SAC_TPS_MIN_TPS / txsPerStep);
    uint32_t maxSteps = std::ceil(
        static_cast<double>(mApp.getConfig().APPLY_LOAD_MAX_SAC_TPS_MAX_TPS) /
        txsPerStep);

    double targetCloseTimeMs = mApp.getConfig().APPLY_LOAD_TARGET_CLOSE_TIME_MS;

    auto txsPerLedgerToTPS =
        [targetCloseTimeMs](uint32_t txsPerLedger) -> uint32_t {
        double targetCloseTimeSec = targetCloseTimeMs / 1000.0;
        return txsPerLedger / targetCloseTimeSec;
    };

    CLOG_WARNING(Perf,
                 "Starting MAX_SAC_TPS binary search between {} and {} TPS "
                 "with search step of {} txs",
                 txsPerLedgerToTPS(minSteps * txsPerStep),
                 txsPerLedgerToTPS(maxSteps * txsPerStep), txsPerStep);
    CLOG_WARNING(Perf, "Target close time: {}ms", targetCloseTimeMs);
    CLOG_WARNING(Perf, "Num parallel clusters: {}",
                 mApp.getConfig().APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS);

    auto prepareIter = [this, txsPerStep](uint32_t numSteps) {
        uint32_t testTxRate = numSteps * txsPerStep;
        uint32_t txsPerLedger =
            testTxRate / mApp.getConfig().APPLY_LOAD_BATCH_SAC_COUNT;

        CLOG_INFO(Perf, "Testing {} TXs per ledger ({} transfers).",
                  txsPerLedger,
                  txsPerLedger * mApp.getConfig().APPLY_LOAD_BATCH_SAC_COUNT);

        upgradeSettingsForMaxTPS(txsPerLedger);
    };
    // Create benchmark function that returns close time for a given TPS step
    auto benchmarkFunc = [this, txsPerStep](uint32_t numSteps) -> double {
        uint32_t testTxRate = numSteps * txsPerStep;
        uint32_t txsPerLedger =
            testTxRate / mApp.getConfig().APPLY_LOAD_BATCH_SAC_COUNT;
        return benchmarkModelTxTpsSingleLedger(ApplyLoadModelTx::SAC,
                                               txsPerLedger);
    };

    size_t maxSamplesPerPoint = mApp.getConfig().APPLY_LOAD_NUM_LEDGERS;
    uint32_t const tolerance = 0;

    auto [lo, hi] =
        noisyBinarySearch(benchmarkFunc, targetCloseTimeMs, minSteps, maxSteps,
                          NOISY_BINARY_SEARCH_CONFIDENCE, tolerance,
                          maxSamplesPerPoint, prepareIter);
    releaseAssert(lo == hi);
    uint32_t bestTxRate = lo * txsPerStep;
    uint32_t bestTxsPerLedger =
        bestTxRate / mApp.getConfig().APPLY_LOAD_BATCH_SAC_COUNT;
    uint32_t bestTps = txsPerLedgerToTPS(
        bestTxsPerLedger * mApp.getConfig().APPLY_LOAD_BATCH_SAC_COUNT);

    CLOG_WARNING(Perf, "================================================");
    CLOG_WARNING(Perf, "Maximum sustainable SAC payments per second: {}",
                 bestTps);
    CLOG_WARNING(Perf, "With parallelism constraint of {} clusters",
                 mApp.getConfig().APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS);
    CLOG_WARNING(Perf, "================================================");
}

void
ApplyLoad::benchmarkModelTx()
{
    releaseAssertOrThrow(mMode == ApplyLoadMode::BENCHMARK_MODEL_TX);

    auto const& config = mApp.getConfig();
    std::vector<double> closeTimes;
    closeTimes.reserve(config.APPLY_LOAD_NUM_LEDGERS);

    // Per-phase timing vectors
    using Timings = LedgerManagerImpl::LedgerClosePhaseTimings;
    std::vector<Timings> allPhaseTimings;
    allPhaseTimings.reserve(config.APPLY_LOAD_NUM_LEDGERS);
    std::vector<TxSetBuildPhaseTimings> allTxSetBuildTimings;
    allTxSetBuildTimings.reserve(config.APPLY_LOAD_NUM_LEDGERS);

    CLOG_WARNING(Perf,
                 "Starting model transaction benchmark for {} ledgers with "
                 "{} tx per ledger",
                 config.APPLY_LOAD_NUM_LEDGERS,
                 config.APPLY_LOAD_MAX_SOROBAN_TX_COUNT);

    auto& lm = static_cast<LedgerManagerImpl&>(mApp.getLedgerManager());

    for (size_t i = 0; i < config.APPLY_LOAD_NUM_LEDGERS; ++i)
    {
        double closeTimeMs = 0.0;
        TxSetBuildPhaseTimings txSetBuildTimings;
        switch (mModelTx)
        {
        case ApplyLoadModelTx::SAC:
            closeTimeMs = benchmarkModelTxTpsSingleLedger(
                ApplyLoadModelTx::SAC, calculateBenchmarkModelTxCount(),
                &txSetBuildTimings);
            break;
        case ApplyLoadModelTx::CUSTOM_TOKEN:
            closeTimeMs = benchmarkModelTxTpsSingleLedger(
                ApplyLoadModelTx::CUSTOM_TOKEN,
                calculateBenchmarkModelTxCount(), &txSetBuildTimings);
            break;
        case ApplyLoadModelTx::SOROSWAP:
            closeTimeMs = benchmarkModelTxTpsSingleLedger(
                ApplyLoadModelTx::SOROSWAP, calculateBenchmarkModelTxCount(),
                &txSetBuildTimings);
            break;
        }
        closeTimes.emplace_back(closeTimeMs);
        allPhaseTimings.emplace_back(lm.getLastPhaseTimings());
        allTxSetBuildTimings.emplace_back(txSetBuildTimings);
    }

    releaseAssert(!closeTimes.empty());

    double avgCloseTimeMs =
        std::accumulate(closeTimes.begin(), closeTimes.end(), 0.0) /
        closeTimes.size();

    double varianceMsSq = 0.0;
    for (auto const& closeTime : closeTimes)
    {
        double delta = closeTime - avgCloseTimeMs;
        varianceMsSq += delta * delta;
    }
    varianceMsSq /= closeTimes.size();

    std::vector<double> sortedCloseTimes = closeTimes;
    std::sort(sortedCloseTimes.begin(), sortedCloseTimes.end());

    CLOG_WARNING(Perf, "================================================");
    CLOG_WARNING(
        Perf, "Model tx benchmark stats ({} ledgers, {} tx per ledger):",
        config.APPLY_LOAD_NUM_LEDGERS, config.APPLY_LOAD_MAX_SOROBAN_TX_COUNT);
    CLOG_WARNING(Perf, "mean close time: {} ms", avgCloseTimeMs);
    CLOG_WARNING(Perf, "p25 close time:  {} ms",
                 interpolatePercentile(sortedCloseTimes, 25.0));
    CLOG_WARNING(Perf, "p50 close time:  {} ms",
                 interpolatePercentile(sortedCloseTimes, 50.0));
    CLOG_WARNING(Perf, "p75 close time:  {} ms",
                 interpolatePercentile(sortedCloseTimes, 75.0));
    CLOG_WARNING(Perf, "p95 close time:  {} ms",
                 interpolatePercentile(sortedCloseTimes, 95.0));
    CLOG_WARNING(Perf, "p99 close time:  {} ms",
                 interpolatePercentile(sortedCloseTimes, 99.0));
    CLOG_WARNING(Perf, "close time stddev: {} ms", std::sqrt(varianceMsSq));
    CLOG_WARNING(Perf, "================================================");

    // Compute and output per-phase statistics table.
    logPhaseTimingsTable(allPhaseTimings);
    logTxSetBuildTimingsTable(allTxSetBuildTimings);
}

double
ApplyLoad::benchmarkModelTxTpsSingleLedger(ApplyLoadModelTx modelTx,
                                           uint32_t txsPerLedger,
                                           TxSetBuildPhaseTimings*
                                               txSetBuildTimings)
{
    auto& totalTxApplyTimer =
        mApp.getConfig().APPLY_LOAD_TIME_WRITES
            ? mApp.getMetrics().NewTimer({"ledger", "ledger", "close"})
            : mApp.getMetrics().NewTimer(
                  {"ledger", "transaction", "total-apply"});

    warmAccountCache();

    int64_t initialSuccessCount = mTxGenerator.getApplySorobanSuccess().count();

    // Generate classic payments using accounts at the end of the range,
    // so they don't overlap with soroban accounts.
    std::vector<TransactionFrameBasePtr> txs;
    txs.reserve(txsPerLedger +
                mApp.getConfig().APPLY_LOAD_CLASSIC_TXS_PER_LEDGER);
    uint32_t classicStartIdx =
        mNumAccounts - mApp.getConfig().APPLY_LOAD_CLASSIC_TXS_PER_LEDGER;
    generateClassicPayments(txs, classicStartIdx);

    // Generate soroban model transactions
    switch (modelTx)
    {
    case ApplyLoadModelTx::SAC:
        generateSacPayments(txs, txsPerLedger);
        break;
    case ApplyLoadModelTx::CUSTOM_TOKEN:
        generateTokenTransfers(txs, txsPerLedger);
        break;
    case ApplyLoadModelTx::SOROSWAP:
        generateSoroswapSwaps(txs, txsPerLedger);
        break;
    }
    releaseAssertOrThrow(
        txs.size() ==
        txsPerLedger + mApp.getConfig().APPLY_LOAD_CLASSIC_TXS_PER_LEDGER);

    mApp.getBucketManager().getLiveBucketList().resolveAllFutures();
    releaseAssert(
        mApp.getBucketManager().getLiveBucketList().futuresAllResolved());
    mApp.getBucketManager().getHotArchiveBucketList().resolveAllFutures();
    releaseAssert(
        mApp.getBucketManager().getHotArchiveBucketList().futuresAllResolved());
    double timeBefore = totalTxApplyTimer.sum();
    closeLedger(txs, {}, false, txSetBuildTimings);
    double timeAfter = totalTxApplyTimer.sum();

    double closeTime = timeAfter - timeBefore;

    CLOG_INFO(Perf, "Model tx benchmark: {:.2f}ms", closeTime);

    // Check transaction success rate. We should never have any failures,
    // and all TXs should have been executed.
    int64_t newSuccessCount =
        mTxGenerator.getApplySorobanSuccess().count() - initialSuccessCount;

    releaseAssert(mTxGenerator.getApplySorobanFailure().count() == 0);
    releaseAssert(newSuccessCount == txsPerLedger);

    // Verify we had max parallelism, i.e. 1 stage with
    // maxDependentTxClusters clusters
    auto& stagesMetric =
        mApp.getMetrics().NewCounter({"ledger", "apply-soroban", "stages"});
    auto& maxClustersMetric = mApp.getMetrics().NewCounter(
        {"ledger", "apply-soroban", "max-clusters"});

    releaseAssert(stagesMetric.count() == 1);
    releaseAssert(maxClustersMetric.count() ==
                  mApp.getConfig().APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS);

    return closeTime;
}

void
ApplyLoad::generateClassicPayments(std::vector<TransactionFrameBasePtr>& txs,
                                   uint32_t startAccountIdx)
{
    auto const& config = mApp.getConfig();
    auto const& accounts = mTxGenerator.getAccounts();
    auto& lm = mApp.getLedgerManager();

    releaseAssert(accounts.size() >=
                  startAccountIdx + config.APPLY_LOAD_CLASSIC_TXS_PER_LEDGER);

    LedgerSnapshot ls(mApp);
    auto appConnector = mApp.getAppConnector();
    auto diagnostics = DiagnosticEventManager::createDisabled();

    for (uint32_t i = 0; i < config.APPLY_LOAD_CLASSIC_TXS_PER_LEDGER; ++i)
    {
        uint64_t accountIdx = startAccountIdx + i;
        auto it = accounts.find(accountIdx);
        releaseAssert(it != accounts.end());
        it->second->loadSequenceNumber();
        auto [_, tx] = mTxGenerator.paymentTransaction(
            mNumAccounts, 0, lm.getLastClosedLedgerNum() + 1, it->first, 1,
            std::nullopt);
        auto res = tx->checkValid(appConnector, ls, 0, 0, 0, diagnostics);
        releaseAssert(res && res->isSuccess());
        txs.emplace_back(tx);
    }
}

void
ApplyLoad::generateSacPayments(std::vector<TransactionFrameBasePtr>& txs,
                               uint32_t count)
{
    auto const& accounts = mTxGenerator.getAccounts();
    auto& lm = mApp.getLedgerManager();

    releaseAssert(accounts.size() >= count);

    // Use batch_transfer
    uint32_t batchSize = mApp.getConfig().APPLY_LOAD_BATCH_SAC_COUNT;
    if (batchSize > 1)
    {
        uint32_t numClusters =
            mApp.getConfig().APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS;
        releaseAssert(mBatchTransferInstances.size() == numClusters);

        // Calculate how many batch transfer transactions we need. Wrt to TPS,
        // here we consider one transfer a "transaction"
        uint32_t txsPerCluster = count / numClusters;
        releaseAssertOrThrow(count % numClusters == 0);

        for (uint32_t clusterId = 0; clusterId < numClusters; ++clusterId)
        {
            for (uint32_t i = 0; i < txsPerCluster; ++i)
            {
                // Use a different source account for each transaction to avoid
                // conflicts
                uint32_t accountIdx =
                    (clusterId * txsPerCluster + i) % mNumAccounts;

                auto it = accounts.find(accountIdx);
                releaseAssert(it != accounts.end());

                // Make sure all destination addresses are unique to avoid rw
                // conflicts
                std::vector<SCAddress> destinations;
                destinations.reserve(batchSize);
                for (uint32_t j = 0; j < batchSize; ++j)
                {
                    SCAddress dest(SC_ADDRESS_TYPE_CONTRACT);
                    dest.contractId() = sha256(std::to_string(mDestCounter++));
                    destinations.push_back(dest);
                }

                // Create batch transfer transaction
                auto tx = mTxGenerator.invokeBatchTransfer(
                    lm.getLastClosedLedgerNum() + 1, accountIdx,
                    mBatchTransferInstances[clusterId], mSACInstanceXLM,
                    destinations);

                txs.push_back(tx.second);
            }
        }
    }
    else
    {
        // Individual transfers via direct SAC invocation
        for (uint32_t i = 0; i < count; ++i)
        {
            SCAddress toAddress(SC_ADDRESS_TYPE_CONTRACT);
            toAddress.contractId() = sha256(
                fmt::format("dest_{}_{}", i, lm.getLastClosedLedgerNum()));

            // Use a different account for each transaction to avoid conflicts
            uint32_t accountIdx = i % mNumAccounts;
            auto it = accounts.find(accountIdx);
            releaseAssert(it != accounts.end());

            auto tx = mTxGenerator.invokeSACPayment(
                lm.getLastClosedLedgerNum() + 1, accountIdx, toAddress,
                mSACInstanceXLM, 100, 1'000'000);

            txs.push_back(tx.second);
        }
    }
    LedgerSnapshot ls(mApp);
    auto diag = DiagnosticEventManager::createDisabled();
    // Validate all the generated transactions. This serves 2 purposes:
    // - ensure that the tx generator works as expected
    // - prime the signature cache
    // Signature cache priming may not be always desirable, but in reality we
    // expect most of the signatures to be cached by the time we execute the
    // transactions, so excluding the verification from the benchmark is likely
    // more realistic than including it.
    for (auto const& tx : txs)
    {
        releaseAssert(tx->checkValid(mApp.getAppConnector(), ls, 0, 0, 0, diag)
                          ->isSuccess());
    }
}
void
ApplyLoad::setupTokenContract()
{
    auto const& lm = mApp.getLedgerManager();
    int64_t initialSuccessCount = mTxGenerator.getApplySorobanSuccess().count();

    auto wasm = rust_bridge::get_apply_load_token_wasm();
    xdr::opaque_vec<> wasmBytes;
    wasmBytes.assign(wasm.data.begin(), wasm.data.end());

    LedgerKey contractCodeLedgerKey;
    contractCodeLedgerKey.type(CONTRACT_CODE);
    contractCodeLedgerKey.contractCode().hash = sha256(wasmBytes);

    SorobanResources uploadResources;
    uploadResources.instructions = 50'000'000;
    uploadResources.diskReadBytes = wasmBytes.size() + 500;
    uploadResources.writeBytes = wasmBytes.size() + 500;

    auto uploadTx = mTxGenerator.createUploadWasmTransaction(
        lm.getLastClosedLedgerNum() + 1, TxGenerator::ROOT_ACCOUNT_ID,
        wasmBytes, contractCodeLedgerKey, std::nullopt, uploadResources);

    closeLedger({uploadTx.second});

    // Create the contract with constructor(owner).
    // The owner is the root account.
    auto rootAccount = mTxGenerator.findAccount(TxGenerator::ROOT_ACCOUNT_ID,
                                                lm.getLastClosedLedgerNum());
    rootAccount->loadSequenceNumber();

    auto salt = sha256("apply load token contract salt");
    auto contractIDPreimage =
        txtest::makeContractIDPreimage(*rootAccount, salt);

    SorobanResources createResources;
    createResources.instructions = 50'000'000;
    createResources.diskReadBytes = wasmBytes.size() + 10000;
    createResources.writeBytes = 50000;

    // Constructor arg: owner address
    SCVal ownerVal(SCV_ADDRESS);
    ownerVal.address() = makeAccountAddress(rootAccount->getPublicKey());

    txtest::ConstructorParams ctorParams;
    ctorParams.constructorArgs = {ownerVal};

    auto createTx = txtest::makeSorobanCreateContractTx(
        mApp, *rootAccount, contractIDPreimage,
        txtest::makeWasmExecutable(contractCodeLedgerKey.contractCode().hash),
        createResources, mTxGenerator.generateFee(std::nullopt, /* opsCnt */ 1),
        ctorParams);
    closeLedger({createTx});

    auto instanceKey = createTx->sorobanResources().footprint.readWrite.back();

    mTokenInstance.readOnlyKeys.emplace_back(contractCodeLedgerKey);
    mTokenInstance.readOnlyKeys.emplace_back(instanceKey);
    mTokenInstance.contractID = instanceKey.contractData().contract;
    mTokenInstance.contractEntriesSize =
        footprintSize(mApp, mTokenInstance.readOnlyKeys);

    // Now call multi_mint to mint tokens to all genesis accounts.
    // Batch into chunks to keep transaction sizes manageable.
    static constexpr uint32_t MINT_BATCH_SIZE = 500;
    uint32_t totalAccounts = mNumAccounts;
    for (uint32_t offset = 0; offset < totalAccounts; offset += MINT_BATCH_SIZE)
    {
        uint32_t batchEnd = std::min(offset + MINT_BATCH_SIZE, totalAccounts);

        auto mintAccount = mTxGenerator.findAccount(
            TxGenerator::ROOT_ACCOUNT_ID, lm.getLastClosedLedgerNum());
        mintAccount->loadSequenceNumber();

        // Build multi_mint invocation: multi_mint(accounts, amount)
        Operation op;
        op.body.type(INVOKE_HOST_FUNCTION);
        auto& ihf = op.body.invokeHostFunctionOp().hostFunction;
        ihf.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
        ihf.invokeContract().contractAddress = mTokenInstance.contractID;
        ihf.invokeContract().functionName = "multi_mint";

        // Build accounts vector
        SCVal accountsVec(SCV_VEC);
        accountsVec.vec().activate();
        for (uint32_t i = offset; i < batchEnd; ++i)
        {
            auto acc = mTxGenerator.getAccount(i);
            SCVal addrVal(SCV_ADDRESS);
            addrVal.address() = makeAccountAddress(acc->getPublicKey());
            accountsVec.vec()->push_back(addrVal);
        }

        ihf.invokeContract().args = {accountsVec,
                                     txtest::makeI128(1'000'000'000)};

        SorobanResources resources;
        resources.instructions = 500'000'000;
        resources.diskReadBytes = wasmBytes.size() + 100'000;
        resources.writeBytes = (batchEnd - offset) * 500 + 10000;

        resources.footprint.readOnly.push_back(
            mTokenInstance.readOnlyKeys.at(0));
        // Put instance into RW footprint as OZ token apparently modifies it
        // on mint.
        resources.footprint.readWrite.push_back(
            mTokenInstance.readOnlyKeys.at(1));

        // Source account
        LedgerKey rootKey(ACCOUNT);
        rootKey.account().accountID = mintAccount->getPublicKey();
        resources.footprint.readWrite.emplace_back(rootKey);

        // Balance entries for each account being minted to
        for (uint32_t i = offset; i < batchEnd; ++i)
        {
            auto acc = mTxGenerator.getAccount(i);
            SCVal addrVal(SCV_ADDRESS);
            addrVal.address() = makeAccountAddress(acc->getPublicKey());

            LedgerKey balanceKey(CONTRACT_DATA);
            balanceKey.contractData().contract = mTokenInstance.contractID;
            balanceKey.contractData().key =
                txtest::makeVecSCVal({makeSymbolSCVal("Balance"), addrVal});
            balanceKey.contractData().durability =
                ContractDataDurability::PERSISTENT;
            resources.footprint.readWrite.emplace_back(balanceKey);
        }

        // Auth: source account credentials for owner
        SorobanAuthorizedInvocation invocation;
        invocation.function.type(SOROBAN_AUTHORIZED_FUNCTION_TYPE_CONTRACT_FN);
        invocation.function.contractFn() = ihf.invokeContract();

        SorobanCredentials credentials(SOROBAN_CREDENTIALS_SOURCE_ACCOUNT);
        op.body.invokeHostFunctionOp().auth.emplace_back(credentials,
                                                         invocation);

        auto resourceFee = txtest::sorobanResourceFee(
            mApp, resources, 5000 + (batchEnd - offset) * 100, 200);
        resourceFee += 500'000'000;

        auto tx = txtest::sorobanTransactionFrameFromOps(
            mApp.getNetworkID(), *mintAccount, {op}, {}, resources,
            mTxGenerator.generateFee(std::nullopt, 1), resourceFee);

        closeLedger({tx});
    }

    int64_t totalSetupTxs =
        mTxGenerator.getApplySorobanSuccess().count() - initialSuccessCount;
    // upload + create + multi_mint batches
    uint32_t expectedMintBatches =
        (totalAccounts + MINT_BATCH_SIZE - 1) / MINT_BATCH_SIZE;
    releaseAssert(totalSetupTxs ==
                  static_cast<int64_t>(2 + expectedMintBatches));
    releaseAssert(mTxGenerator.getApplySorobanFailure().count() == 0);

    CLOG_INFO(Perf,
              "Custom token contract setup complete: {} accounts minted in "
              "{} batches",
              totalAccounts, expectedMintBatches);
}

void
ApplyLoad::generateTokenTransfers(std::vector<TransactionFrameBasePtr>& txs,
                                  uint32_t count)
{
    auto& lm = mApp.getLedgerManager();

    releaseAssert(mNumAccounts >= count * 2);

    for (uint32_t i = 0; i < count; ++i)
    {
        // Use pairs of accounts: (2i, 2i+1) to avoid RW conflicts
        uint32_t fromIdx = 2 * i;
        uint32_t toIdx = 2 * i + 1;

        auto tx = mTxGenerator.invokeTokenTransfer(
            lm.getLastClosedLedgerNum() + 1, fromIdx, toIdx, mTokenInstance,
            100, 1'000'000);

        txs.push_back(tx.second);
    }

    LedgerSnapshot ls(mApp);
    auto diag = DiagnosticEventManager::createDisabled();
    for (auto const& tx : txs)
    {
        releaseAssert(tx->checkValid(mApp.getAppConnector(), ls, 0, 0, 0, diag)
                          ->isSuccess());
    }
}

void
ApplyLoad::setupSoroswapContracts()
{
    auto const& lm = mApp.getLedgerManager();
    auto const& config = mApp.getConfig();
    int64_t initialSuccessCount = mTxGenerator.getApplySorobanSuccess().count();

    // Upgrade maxTxSetSize so we can batch up to 10000 classic ops per
    // ledger during setup.
    static constexpr uint32_t SETUP_MAX_TX_SET_SIZE = 10000;
    {
        auto upgrade = xdr::xvector<UpgradeType, 6>{};
        LedgerUpgrade ledgerUpgrade;
        ledgerUpgrade.type(LEDGER_UPGRADE_MAX_TX_SET_SIZE);
        ledgerUpgrade.newMaxTxSetSize() = SETUP_MAX_TX_SET_SIZE;
        auto v = xdr::xdr_to_opaque(ledgerUpgrade);
        upgrade.push_back(UpgradeType{v.begin(), v.end()});
        closeLedger({}, upgrade);
    }

    // Step 1: We create exactly APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS (C)
    // token pairs (one per cluster/bin) so that the tx set builder can assign
    // each pair's transactions to its own bin, achieving maximum parallelism.
    // Using C+1 tokens in a chain gives exactly C pairs: (T0,T1), (T1,T2), ...,
    // (T_{C-1},T_C).
    uint32_t numPairs = config.APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS;
    uint32_t numTokens = numPairs + 1;
    mSoroswapState.numTokens = numTokens;

    CLOG_INFO(Perf, "Soroswap setup: {} tokens, {} pairs for {} clusters",
              numTokens, numPairs, numPairs);

    // Step 2: Create N classic credit assets using root as issuer
    auto rootAccount = mTxGenerator.findAccount(TxGenerator::ROOT_ACCOUNT_ID,
                                                lm.getLastClosedLedgerNum());
    for (uint32_t i = 0; i < numTokens; ++i)
    {
        std::string code = "T" + std::to_string(i);
        mSoroswapState.assets.push_back(
            txtest::makeAsset(rootAccount->getSecretKey(), code));
    }

    // Step 3: Create trustlines for all accounts x all assets.
    // Batch up to 10000 ChangeTrust txs per ledger close.
    CLOG_INFO(Perf,
              "Soroswap setup: creating trustlines for {} accounts x {} "
              "assets",
              mNumAccounts, numTokens);
    for (uint32_t assetIdx = 0; assetIdx < numTokens; ++assetIdx)
    {
        std::vector<TransactionFrameBasePtr> trustlineTxs;
        for (uint32_t accIdx = 1; accIdx < mNumAccounts; ++accIdx)
        {
            auto acc =
                mTxGenerator.findAccount(accIdx, lm.getLastClosedLedgerNum());
            acc->loadSequenceNumber();
            auto op =
                txtest::changeTrust(mSoroswapState.assets[assetIdx], INT64_MAX);
            auto tx =
                mTxGenerator.createTransactionFramePtr(acc, {op}, std::nullopt);
            trustlineTxs.push_back(
                std::const_pointer_cast<TransactionFrameBase>(tx));

            // Close ledger in batches of SETUP_MAX_TX_SET_SIZE
            if (trustlineTxs.size() >= SETUP_MAX_TX_SET_SIZE)
            {
                closeLedger(trustlineTxs);
                trustlineTxs.clear();
            }
        }
        if (!trustlineTxs.empty())
        {
            closeLedger(trustlineTxs);
        }
    }

    // Step 4: Fund all accounts with each asset.
    // Two-phase approach for efficiency:
    //   Phase 1: Root mints to NUM_DISTRIBUTORS "distribution" accounts
    //            (one multi-op tx per asset, closed in a single ledger).
    //   Phase 2: Each distributor pays ~100 target accounts via a multi-op
    //            tx. We batch up to 100 such txs per ledger close, giving
    //            ~10000 ops per ledger.
    static constexpr uint32_t NUM_DISTRIBUTORS = 100;
    static constexpr uint32_t OPS_PER_TX = 100;
    // Total amount each final account should receive.
    static constexpr int64_t AMOUNT_PER_ACCOUNT = 1'000'000'000;

    CLOG_INFO(Perf, "Soroswap setup: funding accounts ({} distributors)",
              NUM_DISTRIBUTORS);

    // Accounts [1 .. NUM_DISTRIBUTORS] are distributors.
    // Accounts [NUM_DISTRIBUTORS+1 .. mNumAccounts-1] are targets.
    uint32_t numTargets = mNumAccounts - 1 - NUM_DISTRIBUTORS;

    for (uint32_t assetIdx = 0; assetIdx < numTokens; ++assetIdx)
    {
        // Phase 1: Root -> distributors (single multi-op tx per asset).
        {
            int64_t amountPerDistributor =
                AMOUNT_PER_ACCOUNT *
                static_cast<int64_t>((numTargets / NUM_DISTRIBUTORS) + 2);
            std::vector<Operation> ops;
            for (uint32_t d = 1; d <= NUM_DISTRIBUTORS; ++d)
            {
                ops.push_back(txtest::payment(
                    mTxGenerator.getAccount(d)->getPublicKey(),
                    mSoroswapState.assets[assetIdx], amountPerDistributor));
            }
            rootAccount = mTxGenerator.findAccount(TxGenerator::ROOT_ACCOUNT_ID,
                                                   lm.getLastClosedLedgerNum());
            rootAccount->loadSequenceNumber();
            auto tx = mTxGenerator.createTransactionFramePtr(rootAccount, ops,
                                                             std::nullopt);
            closeLedger({std::const_pointer_cast<TransactionFrameBase>(tx)});
        }

        // Phase 2: Distributors -> targets.
        // Each distributor handles a slice of target accounts.
        // Build one multi-op tx per distributor, batch up to 100 txs per
        // ledger close (~10000 ops per ledger).
        uint32_t firstTarget = NUM_DISTRIBUTORS + 1;

        // Group targets by distributor (round-robin assignment).
        std::vector<std::vector<uint32_t>> distTargets(NUM_DISTRIBUTORS);
        for (uint32_t targetIdx = firstTarget; targetIdx < mNumAccounts;
             ++targetIdx)
        {
            uint32_t distSlot = (targetIdx - firstTarget) % NUM_DISTRIBUTORS;
            distTargets[distSlot].push_back(targetIdx);
        }

        // Build txs: one tx per OPS_PER_TX targets of a distributor.
        std::vector<TransactionFrameBasePtr> batchTxs;
        for (uint32_t d = 0; d < NUM_DISTRIBUTORS; ++d)
        {
            uint32_t distAccId = d + 1;
            auto const& targets = distTargets[d];
            std::vector<Operation> ops;
            for (size_t t = 0; t < targets.size(); ++t)
            {
                ops.push_back(txtest::payment(
                    mTxGenerator.getAccount(targets[t])->getPublicKey(),
                    mSoroswapState.assets[assetIdx], AMOUNT_PER_ACCOUNT));

                if (ops.size() >= OPS_PER_TX || t == targets.size() - 1)
                {
                    auto distAcc = mTxGenerator.findAccount(
                        distAccId, lm.getLastClosedLedgerNum());
                    distAcc->loadSequenceNumber();
                    auto tx = mTxGenerator.createTransactionFramePtr(
                        distAcc, ops, std::nullopt);
                    batchTxs.push_back(
                        std::const_pointer_cast<TransactionFrameBase>(tx));
                    ops.clear();

                    if (batchTxs.size() >= 100)
                    {
                        closeLedger(batchTxs);
                        batchTxs.clear();
                    }
                }
            }
        }
        if (!batchTxs.empty())
        {
            closeLedger(batchTxs);
        }
    }

    // Step 5: Create N SAC contracts for each asset.
    // We use higher resource limits than createSACTransaction's defaults
    // because credit asset SAC initialization needs more than 1M
    // instructions.
    CLOG_INFO(Perf, "Soroswap setup: creating {} SAC contracts", numTokens);
    mSoroswapState.sacInstances.resize(numTokens);
    for (uint32_t i = 0; i < numTokens; ++i)
    {
        rootAccount = mTxGenerator.findAccount(TxGenerator::ROOT_ACCOUNT_ID,
                                               lm.getLastClosedLedgerNum());
        rootAccount->loadSequenceNumber();

        SorobanResources sacResources;
        sacResources.instructions = 10'000'000;
        sacResources.diskReadBytes = 1000;
        sacResources.writeBytes = 1000;

        auto contractIDPreimage =
            txtest::makeContractIDPreimage(mSoroswapState.assets[i]);

        auto createTx = txtest::makeSorobanCreateContractTx(
            mApp, *rootAccount, contractIDPreimage,
            txtest::makeAssetExecutable(mSoroswapState.assets[i]), sacResources,
            mTxGenerator.generateFee(std::nullopt, /* opsCnt */ 1));
        closeLedger({createTx});

        auto instanceKey =
            createTx->sorobanResources().footprint.readWrite.back();
        mSoroswapState.sacInstances[i].readOnlyKeys.emplace_back(instanceKey);
        mSoroswapState.sacInstances[i].contractID =
            instanceKey.contractData().contract;
    }

    // Step 6: Upload 3 Soroswap Wasms (factory, pair, router)
    CLOG_INFO(Perf, "Soroswap setup: uploading Wasms");

    auto factoryWasm = rust_bridge::get_apply_load_soroswap_factory_wasm();
    xdr::opaque_vec<> factoryWasmBytes;
    factoryWasmBytes.assign(factoryWasm.data.begin(), factoryWasm.data.end());
    LedgerKey factoryCodeKey;
    factoryCodeKey.type(CONTRACT_CODE);
    factoryCodeKey.contractCode().hash = sha256(factoryWasmBytes);
    mSoroswapState.factoryCodeKey = factoryCodeKey;

    SorobanResources factoryUploadRes;
    factoryUploadRes.instructions = 50'000'000;
    factoryUploadRes.diskReadBytes =
        static_cast<uint32_t>(factoryWasmBytes.size()) + 500;
    factoryUploadRes.writeBytes =
        static_cast<uint32_t>(factoryWasmBytes.size()) + 500;
    auto factoryUploadTx = mTxGenerator.createUploadWasmTransaction(
        lm.getLastClosedLedgerNum() + 1, TxGenerator::ROOT_ACCOUNT_ID,
        factoryWasmBytes, factoryCodeKey, std::nullopt, factoryUploadRes);
    closeLedger({factoryUploadTx.second});

    auto pairWasm = rust_bridge::get_apply_load_soroswap_pool_wasm();
    xdr::opaque_vec<> pairWasmBytes;
    pairWasmBytes.assign(pairWasm.data.begin(), pairWasm.data.end());
    LedgerKey pairCodeKey;
    pairCodeKey.type(CONTRACT_CODE);
    pairCodeKey.contractCode().hash = sha256(pairWasmBytes);
    mSoroswapState.pairCodeKey = pairCodeKey;

    SorobanResources pairUploadRes;
    pairUploadRes.instructions = 50'000'000;
    pairUploadRes.diskReadBytes =
        static_cast<uint32_t>(pairWasmBytes.size()) + 500;
    pairUploadRes.writeBytes =
        static_cast<uint32_t>(pairWasmBytes.size()) + 500;
    auto pairUploadTx = mTxGenerator.createUploadWasmTransaction(
        lm.getLastClosedLedgerNum() + 1, TxGenerator::ROOT_ACCOUNT_ID,
        pairWasmBytes, pairCodeKey, std::nullopt, pairUploadRes);
    closeLedger({pairUploadTx.second});

    auto routerWasm = rust_bridge::get_apply_load_soroswap_router_wasm();
    xdr::opaque_vec<> routerWasmBytes;
    routerWasmBytes.assign(routerWasm.data.begin(), routerWasm.data.end());
    LedgerKey routerCodeKey;
    routerCodeKey.type(CONTRACT_CODE);
    routerCodeKey.contractCode().hash = sha256(routerWasmBytes);
    mSoroswapState.routerCodeKey = routerCodeKey;

    SorobanResources routerUploadRes;
    routerUploadRes.instructions = 50'000'000;
    routerUploadRes.diskReadBytes =
        static_cast<uint32_t>(routerWasmBytes.size()) + 500;
    routerUploadRes.writeBytes =
        static_cast<uint32_t>(routerWasmBytes.size()) + 500;
    auto routerUploadTx = mTxGenerator.createUploadWasmTransaction(
        lm.getLastClosedLedgerNum() + 1, TxGenerator::ROOT_ACCOUNT_ID,
        routerWasmBytes, routerCodeKey, std::nullopt, routerUploadRes);
    closeLedger({routerUploadTx.second});

    // Step 7: Deploy factory contract and initialize it
    CLOG_INFO(Perf, "Soroswap setup: deploying factory");
    {
        rootAccount = mTxGenerator.findAccount(TxGenerator::ROOT_ACCOUNT_ID,
                                               lm.getLastClosedLedgerNum());
        rootAccount->loadSequenceNumber();

        auto salt = sha256("soroswap factory salt");
        auto contractIDPreimage =
            txtest::makeContractIDPreimage(*rootAccount, salt);

        SorobanResources createResources;
        createResources.instructions = 50'000'000;
        createResources.diskReadBytes =
            static_cast<uint32_t>(factoryWasmBytes.size()) + 10000;
        createResources.writeBytes = 50000;

        auto createTx = txtest::makeSorobanCreateContractTx(
            mApp, *rootAccount, contractIDPreimage,
            txtest::makeWasmExecutable(factoryCodeKey.contractCode().hash),
            createResources,
            mTxGenerator.generateFee(std::nullopt, /* opsCnt */ 1));
        closeLedger({createTx});

        auto instanceKey =
            createTx->sorobanResources().footprint.readWrite.back();
        mSoroswapState.factoryInstanceKey = instanceKey;
        mSoroswapState.factoryContractID = instanceKey.contractData().contract;
    }

    // Initialize factory: initialize(setter, pair_wasm_hash)
    CLOG_INFO(Perf, "Soroswap setup: initializing factory");
    {
        rootAccount = mTxGenerator.findAccount(TxGenerator::ROOT_ACCOUNT_ID,
                                               lm.getLastClosedLedgerNum());
        rootAccount->loadSequenceNumber();

        auto setterVal =
            makeAddressSCVal(makeAccountAddress(rootAccount->getPublicKey()));

        SCVal pairWasmHashVal(SCV_BYTES);
        pairWasmHashVal.bytes().assign(pairCodeKey.contractCode().hash.begin(),
                                       pairCodeKey.contractCode().hash.end());

        Operation op;
        op.body.type(INVOKE_HOST_FUNCTION);
        auto& ihf = op.body.invokeHostFunctionOp().hostFunction;
        ihf.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
        ihf.invokeContract().contractAddress = mSoroswapState.factoryContractID;
        ihf.invokeContract().functionName = "initialize";
        ihf.invokeContract().args = {setterVal, pairWasmHashVal};

        SorobanResources resources;
        resources.instructions = 50'000'000;
        resources.diskReadBytes =
            static_cast<uint32_t>(factoryWasmBytes.size()) + 10000;
        resources.writeBytes = 50000;
        resources.footprint.readOnly.push_back(factoryCodeKey);
        resources.footprint.readWrite.push_back(
            mSoroswapState.factoryInstanceKey);

        // PairWasmHash persistent data key (factory.initialize writes this)
        {
            LedgerKey pairWasmHashDataKey(CONTRACT_DATA);
            pairWasmHashDataKey.contractData().contract =
                mSoroswapState.factoryContractID;
            pairWasmHashDataKey.contractData().key =
                txtest::makeVecSCVal({makeSymbolSCVal("PairWasmHash")});
            pairWasmHashDataKey.contractData().durability =
                ContractDataDurability::PERSISTENT;
            resources.footprint.readWrite.push_back(pairWasmHashDataKey);
        }

        // Source account for auth
        SorobanAuthorizedInvocation invocation;
        invocation.function.type(SOROBAN_AUTHORIZED_FUNCTION_TYPE_CONTRACT_FN);
        invocation.function.contractFn() = ihf.invokeContract();
        SorobanCredentials credentials(SOROBAN_CREDENTIALS_SOURCE_ACCOUNT);
        op.body.invokeHostFunctionOp().auth.emplace_back(credentials,
                                                         invocation);

        auto resourceFee =
            txtest::sorobanResourceFee(mApp, resources, 5000, 200);
        resourceFee += 50'000'000;

        auto tx = txtest::sorobanTransactionFrameFromOps(
            mApp.getNetworkID(), *rootAccount, {op}, {}, resources,
            mTxGenerator.generateFee(std::nullopt, 1), resourceFee);
        closeLedger({tx});
    }

    // Step 8: Deploy router contract and initialize it
    CLOG_INFO(Perf, "Soroswap setup: deploying router");
    {
        rootAccount = mTxGenerator.findAccount(TxGenerator::ROOT_ACCOUNT_ID,
                                               lm.getLastClosedLedgerNum());
        rootAccount->loadSequenceNumber();

        auto salt = sha256("soroswap router salt");
        auto contractIDPreimage =
            txtest::makeContractIDPreimage(*rootAccount, salt);

        SorobanResources createResources;
        createResources.instructions = 50'000'000;
        createResources.diskReadBytes =
            static_cast<uint32_t>(routerWasmBytes.size()) + 10000;
        createResources.writeBytes = 50000;

        auto createTx = txtest::makeSorobanCreateContractTx(
            mApp, *rootAccount, contractIDPreimage,
            txtest::makeWasmExecutable(routerCodeKey.contractCode().hash),
            createResources,
            mTxGenerator.generateFee(std::nullopt, /* opsCnt */ 1));
        closeLedger({createTx});

        auto instanceKey =
            createTx->sorobanResources().footprint.readWrite.back();
        mSoroswapState.routerInstanceKey = instanceKey;
        mSoroswapState.routerContractID = instanceKey.contractData().contract;
    }

    // Initialize router: initialize(factory_address)
    CLOG_INFO(Perf, "Soroswap setup: initializing router");
    {
        rootAccount = mTxGenerator.findAccount(TxGenerator::ROOT_ACCOUNT_ID,
                                               lm.getLastClosedLedgerNum());
        rootAccount->loadSequenceNumber();

        auto factoryVal = makeAddressSCVal(mSoroswapState.factoryContractID);

        Operation op;
        op.body.type(INVOKE_HOST_FUNCTION);
        auto& ihf = op.body.invokeHostFunctionOp().hostFunction;
        ihf.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
        ihf.invokeContract().contractAddress = mSoroswapState.routerContractID;
        ihf.invokeContract().functionName = "initialize";
        ihf.invokeContract().args = {factoryVal};

        SorobanResources resources;
        resources.instructions = 50'000'000;
        resources.diskReadBytes =
            static_cast<uint32_t>(routerWasmBytes.size()) + 10000;
        resources.writeBytes = 50000;
        resources.footprint.readOnly.push_back(routerCodeKey);
        resources.footprint.readWrite.push_back(
            mSoroswapState.routerInstanceKey);

        SorobanAuthorizedInvocation invocation;
        invocation.function.type(SOROBAN_AUTHORIZED_FUNCTION_TYPE_CONTRACT_FN);
        invocation.function.contractFn() = ihf.invokeContract();
        SorobanCredentials credentials(SOROBAN_CREDENTIALS_SOURCE_ACCOUNT);
        op.body.invokeHostFunctionOp().auth.emplace_back(credentials,
                                                         invocation);

        auto resourceFee =
            txtest::sorobanResourceFee(mApp, resources, 5000, 200);
        resourceFee += 50'000'000;

        auto tx = txtest::sorobanTransactionFrameFromOps(
            mApp.getNetworkID(), *rootAccount, {op}, {}, resources,
            mTxGenerator.generateFee(std::nullopt, 1), resourceFee);
        closeLedger({tx});
    }

    // Step 9: Create pairs explicitly via factory.create_pair().
    // We compute each pair's contract address deterministically so we can
    // build the correct footprint before submission.
    CLOG_INFO(Perf, "Soroswap setup: creating {} pairs via factory", numPairs);
    for (uint32_t pairNum = 0; pairNum < numPairs; ++pairNum)
    {
        // Chain: pair pairNum uses tokens (pairNum, pairNum+1)
        uint32_t i = pairNum;
        uint32_t j = pairNum + 1;

        rootAccount = mTxGenerator.findAccount(TxGenerator::ROOT_ACCOUNT_ID,
                                               lm.getLastClosedLedgerNum());
        rootAccount->loadSequenceNumber();

        // Sort tokens as Soroswap factory does (token_0 < token_1)
        SCAddress token0 = mSoroswapState.sacInstances[i].contractID;
        SCAddress token1 = mSoroswapState.sacInstances[j].contractID;
        if (token1 < token0)
            std::swap(token0, token1);

        // Compute pair salt: sha256(xdr(ScVal(token0)) ||
        // xdr(ScVal(token1))). This matches Soroban SDK's
        // Address::to_xdr() used in factory's pair.rs salt().
        auto token0Val = makeAddressSCVal(token0);
        auto token1Val = makeAddressSCVal(token1);
        auto xdr0 = xdr::xdr_to_opaque(token0Val);
        auto xdr1 = xdr::xdr_to_opaque(token1Val);
        std::vector<uint8_t> saltInput(xdr0.begin(), xdr0.end());
        saltInput.insert(saltInput.end(), xdr1.begin(), xdr1.end());
        uint256 pairSalt =
            sha256(ByteSlice(saltInput.data(), saltInput.size()));

        // Derive pair contract address deterministically
        ContractIDPreimage pairPreimage(CONTRACT_ID_PREIMAGE_FROM_ADDRESS);
        pairPreimage.fromAddress().address = mSoroswapState.factoryContractID;
        pairPreimage.fromAddress().salt = pairSalt;
        auto fullPreimage = txtest::makeFullContractIdPreimage(
            mApp.getNetworkID(), pairPreimage);
        Hash pairContractHash = xdrSha256(fullPreimage);
        SCAddress pairAddress = txtest::makeContractAddress(pairContractHash);
        LedgerKey pairInstanceKey =
            txtest::makeContractInstanceKey(pairAddress);

        // Store pair info
        SoroswapPairInfo pairInfo;
        pairInfo.tokenAIndex = i;
        pairInfo.tokenBIndex = j;
        pairInfo.pairContractID = pairAddress;
        mSoroswapState.pairs.push_back(pairInfo);
        uint32_t pairIdx =
            static_cast<uint32_t>(mSoroswapState.pairs.size() - 1);

        // Build factory.create_pair(token_a, token_b) invocation
        auto tokenAVal =
            makeAddressSCVal(mSoroswapState.sacInstances[i].contractID);
        auto tokenBVal =
            makeAddressSCVal(mSoroswapState.sacInstances[j].contractID);

        Operation op;
        op.body.type(INVOKE_HOST_FUNCTION);
        auto& ihf = op.body.invokeHostFunctionOp().hostFunction;
        ihf.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
        ihf.invokeContract().contractAddress = mSoroswapState.factoryContractID;
        ihf.invokeContract().functionName = "create_pair";
        ihf.invokeContract().args = {tokenAVal, tokenBVal};

        SorobanResources resources;
        resources.instructions = 100'000'000;
        resources.diskReadBytes = 100'000;
        resources.writeBytes = 100'000;

        // Read-only: factory code, pair Wasm code,
        //            PairWasmHash (persistent, read during deploy),
        //            SAC token instances (pair.initialize calls
        //            token_0.symbol() and token_1.symbol())
        resources.footprint.readOnly.push_back(factoryCodeKey);
        resources.footprint.readOnly.push_back(pairCodeKey);
        resources.footprint.readOnly.push_back(
            mSoroswapState.sacInstances[i].readOnlyKeys.at(0));
        resources.footprint.readOnly.push_back(
            mSoroswapState.sacInstances[j].readOnlyKeys.at(0));
        {
            LedgerKey pairWasmHashKey(CONTRACT_DATA);
            pairWasmHashKey.contractData().contract =
                mSoroswapState.factoryContractID;
            pairWasmHashKey.contractData().key =
                txtest::makeVecSCVal({makeSymbolSCVal("PairWasmHash")});
            pairWasmHashKey.contractData().durability =
                ContractDataDurability::PERSISTENT;
            resources.footprint.readOnly.push_back(pairWasmHashKey);
        }

        // Read-write: factory instance (TotalPairs update),
        //             new pair instance (created),
        //             PairAddressesByTokens (created),
        //             PairAddressesNIndexed(n) (created)
        resources.footprint.readWrite.push_back(
            mSoroswapState.factoryInstanceKey);
        resources.footprint.readWrite.push_back(pairInstanceKey);
        {
            LedgerKey pairByTokensLK(CONTRACT_DATA);
            pairByTokensLK.contractData().contract =
                mSoroswapState.factoryContractID;
            pairByTokensLK.contractData().key = txtest::makeVecSCVal(
                {makeSymbolSCVal("PairAddressesByTokens"),
                 txtest::makeVecSCVal({token0Val, token1Val})});
            pairByTokensLK.contractData().durability =
                ContractDataDurability::PERSISTENT;
            resources.footprint.readWrite.push_back(pairByTokensLK);
        }
        {
            LedgerKey nIndexedLK(CONTRACT_DATA);
            nIndexedLK.contractData().contract =
                mSoroswapState.factoryContractID;
            nIndexedLK.contractData().key =
                txtest::makeVecSCVal({makeSymbolSCVal("PairAddressesNIndexed"),
                                      txtest::makeU32(pairIdx)});
            nIndexedLK.contractData().durability =
                ContractDataDurability::PERSISTENT;
            resources.footprint.readWrite.push_back(nIndexedLK);
        }

        // factory.create_pair doesn't call require_auth
        auto resourceFee =
            txtest::sorobanResourceFee(mApp, resources, 20000, 200);
        resourceFee += 500'000'000;

        auto tx = txtest::sorobanTransactionFrameFromOps(
            mApp.getNetworkID(), *rootAccount, {op}, {}, resources,
            mTxGenerator.generateFee(std::nullopt, 1), resourceFee);
        closeLedger({tx});
    }

    // Step 10: Add liquidity to all pairs via router.add_liquidity.
    // Pairs already exist from step 9, so footprint is simpler.
    CLOG_INFO(Perf, "Soroswap setup: adding liquidity to {} pairs", numPairs);
    for (size_t pairIdx = 0; pairIdx < mSoroswapState.pairs.size(); ++pairIdx)
    {
        auto const& pair = mSoroswapState.pairs[pairIdx];
        uint32_t ti = pair.tokenAIndex;
        uint32_t tj = pair.tokenBIndex;

        rootAccount = mTxGenerator.findAccount(TxGenerator::ROOT_ACCOUNT_ID,
                                               lm.getLastClosedLedgerNum());
        rootAccount->loadSequenceNumber();

        auto tokenAVal =
            makeAddressSCVal(mSoroswapState.sacInstances[ti].contractID);
        auto tokenBVal =
            makeAddressSCVal(mSoroswapState.sacInstances[tj].contractID);

        int64_t desiredAmount = 100'000'000;
        int64_t minAmount = 99'000'000;

        auto toVal =
            makeAddressSCVal(makeAccountAddress(rootAccount->getPublicKey()));

        SCVal deadlineVal(SCV_U64);
        deadlineVal.u64() = UINT64_MAX;

        Operation op;
        op.body.type(INVOKE_HOST_FUNCTION);
        auto& ihf = op.body.invokeHostFunctionOp().hostFunction;
        ihf.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
        ihf.invokeContract().contractAddress = mSoroswapState.routerContractID;
        ihf.invokeContract().functionName = "add_liquidity";
        ihf.invokeContract().args = {tokenAVal,
                                     tokenBVal,
                                     txtest::makeI128(desiredAmount),
                                     txtest::makeI128(desiredAmount),
                                     txtest::makeI128(minAmount),
                                     txtest::makeI128(minAmount),
                                     toVal,
                                     deadlineVal};

        SorobanResources resources;
        resources.instructions = 100'000'000;
        resources.diskReadBytes = 100'000;
        resources.writeBytes = 100'000;

        // Sort tokens for the factory PairAddressesByTokens lookup key
        SCAddress sortedToken0 = mSoroswapState.sacInstances[ti].contractID;
        SCAddress sortedToken1 = mSoroswapState.sacInstances[tj].contractID;
        if (sortedToken1 < sortedToken0)
            std::swap(sortedToken0, sortedToken1);
        auto sortedToken0Val = makeAddressSCVal(sortedToken0);
        auto sortedToken1Val = makeAddressSCVal(sortedToken1);

        auto pairAddrVal = makeAddressSCVal(pair.pairContractID);

        // Read-only: router code+instance, factory code+instance,
        //            PairAddressesByTokens, token SAC instances, pair code
        resources.footprint.readOnly.push_back(routerCodeKey);
        resources.footprint.readOnly.push_back(
            mSoroswapState.routerInstanceKey);
        resources.footprint.readOnly.push_back(factoryCodeKey);
        resources.footprint.readOnly.push_back(
            mSoroswapState.factoryInstanceKey);
        {
            LedgerKey pairByTokensLK(CONTRACT_DATA);
            pairByTokensLK.contractData().contract =
                mSoroswapState.factoryContractID;
            pairByTokensLK.contractData().key = txtest::makeVecSCVal(
                {makeSymbolSCVal("PairAddressesByTokens"),
                 txtest::makeVecSCVal({sortedToken0Val, sortedToken1Val})});
            pairByTokensLK.contractData().durability =
                ContractDataDurability::PERSISTENT;
            resources.footprint.readOnly.push_back(pairByTokensLK);
        }
        resources.footprint.readOnly.push_back(
            mSoroswapState.sacInstances[ti].readOnlyKeys.at(0));
        resources.footprint.readOnly.push_back(
            mSoroswapState.sacInstances[tj].readOnlyKeys.at(0));
        resources.footprint.readOnly.push_back(pairCodeKey);

        // Read-write: root account, trustlines, token balances,
        //             pair instance, LP token balance
        LedgerKey rootKey(ACCOUNT);
        rootKey.account().accountID = rootAccount->getPublicKey();
        resources.footprint.readWrite.emplace_back(rootKey);

        // Note: root is the asset issuer, so no trustline entries are
        // needed — issuers have unlimited supply and no trustlines.

        // Token A Balance[pair]
        resources.footprint.readWrite.emplace_back(makeSACBalanceKey(
            mSoroswapState.sacInstances[ti].contractID, pairAddrVal));
        // Token B Balance[pair]
        resources.footprint.readWrite.emplace_back(makeSACBalanceKey(
            mSoroswapState.sacInstances[tj].contractID, pairAddrVal));
        // Pair contract instance (RW - modified during deposit)
        resources.footprint.readWrite.emplace_back(
            txtest::makeContractInstanceKey(pair.pairContractID));
        // Pair LP token Balance[root] (minted during first deposit)
        resources.footprint.readWrite.emplace_back(
            makeSACBalanceKey(pair.pairContractID, toVal));
        // Pair LP token Balance[pair_contract] (MINIMUM_LIQUIDITY minted
        // to pair itself during first deposit)
        resources.footprint.readWrite.emplace_back(
            makeSACBalanceKey(pair.pairContractID, pairAddrVal));

        // Auth: root authorizes add_liquidity which sub-invokes
        // token_a.transfer and token_b.transfer
        SorobanAuthorizedInvocation rootInvocation;
        rootInvocation.function.type(
            SOROBAN_AUTHORIZED_FUNCTION_TYPE_CONTRACT_FN);
        rootInvocation.function.contractFn() = ihf.invokeContract();

        // Sub-invocation: token_a.transfer(root, pair, amount)
        SorobanAuthorizedInvocation transferAInvocation;
        transferAInvocation.function.type(
            SOROBAN_AUTHORIZED_FUNCTION_TYPE_CONTRACT_FN);
        transferAInvocation.function.contractFn().contractAddress =
            mSoroswapState.sacInstances[ti].contractID;
        transferAInvocation.function.contractFn().functionName = "transfer";
        transferAInvocation.function.contractFn().args = {
            toVal, pairAddrVal, txtest::makeI128(desiredAmount)};

        // Sub-invocation: token_b.transfer(root, pair, amount)
        SorobanAuthorizedInvocation transferBInvocation;
        transferBInvocation.function.type(
            SOROBAN_AUTHORIZED_FUNCTION_TYPE_CONTRACT_FN);
        transferBInvocation.function.contractFn().contractAddress =
            mSoroswapState.sacInstances[tj].contractID;
        transferBInvocation.function.contractFn().functionName = "transfer";
        transferBInvocation.function.contractFn().args = {
            toVal, pairAddrVal, txtest::makeI128(desiredAmount)};

        rootInvocation.subInvocations.push_back(transferAInvocation);
        rootInvocation.subInvocations.push_back(transferBInvocation);

        SorobanCredentials credentials(SOROBAN_CREDENTIALS_SOURCE_ACCOUNT);
        op.body.invokeHostFunctionOp().auth.emplace_back(credentials,
                                                         rootInvocation);

        auto resourceFee =
            txtest::sorobanResourceFee(mApp, resources, 20000, 200);
        resourceFee += 500'000'000;

        auto tx = txtest::sorobanTransactionFrameFromOps(
            mApp.getNetworkID(), *rootAccount, {op}, {}, resources,
            mTxGenerator.generateFee(std::nullopt, 1), resourceFee);
        closeLedger({tx});
    }

    // Initialize swap counters for alternating direction
    mSoroswapSwapCounters.resize(numPairs, 0);

    int64_t totalSetupTxs =
        mTxGenerator.getApplySorobanSuccess().count() - initialSuccessCount;
    // N SAC creates + 3 Wasm uploads + factory create + factory init
    // + router create + router init + numPairs create_pair
    // + numPairs add_liquidity
    int64_t expectedSorobanTxs = numTokens + 3 + 2 + 2 + 2 * numPairs;
    CLOG_INFO(Perf,
              "Soroswap setup complete: {} soroban txs (expected {}), {} "
              "failures",
              totalSetupTxs, expectedSorobanTxs,
              mTxGenerator.getApplySorobanFailure().count());
    releaseAssert(mTxGenerator.getApplySorobanFailure().count() == 0);
}

void
ApplyLoad::generateSoroswapSwaps(std::vector<TransactionFrameBasePtr>& txs,
                                 uint32_t count)
{
    auto& lm = mApp.getLedgerManager();
    uint32_t numPairs = mSoroswapState.pairs.size();
    releaseAssert(numPairs > 0);

    for (uint32_t i = 0; i < count; ++i)
    {
        // Round-robin across pairs for parallelism
        uint32_t pairIndex = i % numPairs;
        auto const& pair = mSoroswapState.pairs[pairIndex];

        // Unique account per tx (skip account 0 = root/issuer)
        uint32_t accountIdx = i + 1;

        // Alternate swap direction per pair to keep pools balanced
        bool swapAForB = (mSoroswapSwapCounters[pairIndex] % 2 == 0);
        mSoroswapSwapCounters[pairIndex]++;

        uint32_t tokenInIdx = swapAForB ? pair.tokenAIndex : pair.tokenBIndex;
        uint32_t tokenOutIdx = swapAForB ? pair.tokenBIndex : pair.tokenAIndex;

        auto fromAccount =
            mTxGenerator.findAccount(accountIdx, lm.getLastClosedLedgerNum());
        fromAccount->loadSequenceNumber();

        auto fromVal =
            makeAddressSCVal(makeAccountAddress(fromAccount->getPublicKey()));

        // Build path: [token_in, token_out]
        auto tokenInVal = makeAddressSCVal(
            mSoroswapState.sacInstances[tokenInIdx].contractID);
        auto tokenOutVal = makeAddressSCVal(
            mSoroswapState.sacInstances[tokenOutIdx].contractID);

        SCVal pathVec(SCV_VEC);
        pathVec.vec().activate();
        pathVec.vec()->push_back(tokenInVal);
        pathVec.vec()->push_back(tokenOutVal);

        int64_t swapAmount = 100;
        SCVal deadlineVal(SCV_U64);
        deadlineVal.u64() = UINT64_MAX;

        Operation op;
        op.body.type(INVOKE_HOST_FUNCTION);
        auto& ihf = op.body.invokeHostFunctionOp().hostFunction;
        ihf.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
        ihf.invokeContract().contractAddress = mSoroswapState.routerContractID;
        ihf.invokeContract().functionName = "swap_exact_tokens_for_tokens";
        ihf.invokeContract().args = {
            txtest::makeI128(swapAmount), // amount_in
            txtest::makeI128(0),          // amount_out_min
            pathVec,                      // path
            fromVal,                      // to
            deadlineVal                   // deadline
        };

        // Footprint
        SorobanResources resources;
        resources.instructions = TxGenerator::SOROSWAP_SWAP_TX_INSTRUCTIONS;
        resources.diskReadBytes = 5000;
        resources.writeBytes = 5000;

        // Read-only: router instance, token_in SAC instance,
        //            token_out SAC instance, router code, pair code
        resources.footprint.readOnly.push_back(
            mSoroswapState.routerInstanceKey);
        resources.footprint.readOnly.push_back(
            mSoroswapState.sacInstances[tokenInIdx].readOnlyKeys.at(0));
        resources.footprint.readOnly.push_back(
            mSoroswapState.sacInstances[tokenOutIdx].readOnlyKeys.at(0));
        resources.footprint.readOnly.push_back(mSoroswapState.routerCodeKey);
        resources.footprint.readOnly.push_back(mSoroswapState.pairCodeKey);

        // Read-write: user trustline(A), user trustline(B),
        //             Balance[pair] for token_in, Balance[pair] for
        //             token_out, pair instance
        resources.footprint.readWrite.emplace_back(makeTrustlineKey(
            fromAccount->getPublicKey(), mSoroswapState.assets[tokenInIdx]));
        resources.footprint.readWrite.emplace_back(makeTrustlineKey(
            fromAccount->getPublicKey(), mSoroswapState.assets[tokenOutIdx]));

        auto pairAddrVal = makeAddressSCVal(pair.pairContractID);
        // Balance[pair] for token_in
        resources.footprint.readWrite.emplace_back(makeSACBalanceKey(
            mSoroswapState.sacInstances[tokenInIdx].contractID, pairAddrVal));
        // Balance[pair] for token_out
        resources.footprint.readWrite.emplace_back(makeSACBalanceKey(
            mSoroswapState.sacInstances[tokenOutIdx].contractID, pairAddrVal));
        // Pair contract instance (RW - modified during swap)
        resources.footprint.readWrite.emplace_back(
            txtest::makeContractInstanceKey(pair.pairContractID));

        // Auth: source_account authorizes swap_exact_tokens_for_tokens
        // which sub-invokes token_in.transfer(user, pair, amount)
        SorobanAuthorizedInvocation rootInvocation;
        rootInvocation.function.type(
            SOROBAN_AUTHORIZED_FUNCTION_TYPE_CONTRACT_FN);
        rootInvocation.function.contractFn() = ihf.invokeContract();

        SorobanAuthorizedInvocation transferInvocation;
        transferInvocation.function.type(
            SOROBAN_AUTHORIZED_FUNCTION_TYPE_CONTRACT_FN);
        transferInvocation.function.contractFn().contractAddress =
            mSoroswapState.sacInstances[tokenInIdx].contractID;
        transferInvocation.function.contractFn().functionName = "transfer";
        transferInvocation.function.contractFn().args = {
            fromVal, pairAddrVal, txtest::makeI128(swapAmount)};
        rootInvocation.subInvocations.push_back(transferInvocation);

        SorobanCredentials credentials(SOROBAN_CREDENTIALS_SOURCE_ACCOUNT);
        op.body.invokeHostFunctionOp().auth.emplace_back(credentials,
                                                         rootInvocation);

        auto resourceFee =
            txtest::sorobanResourceFee(mApp, resources, 1000, 200);
        resourceFee += 5'000'000;

        auto tx = txtest::sorobanTransactionFrameFromOps(
            mApp.getNetworkID(), *fromAccount, {op}, {}, resources,
            mTxGenerator.generateFee(std::nullopt, 1), resourceFee);
        txs.push_back(tx);
    }

    LedgerSnapshot ls(mApp);
    auto diag = DiagnosticEventManager::createDisabled();
    for (auto const& tx : txs)
    {
        releaseAssert(tx->checkValid(mApp.getAppConnector(), ls, 0, 0, 0, diag)
                          ->isSuccess());
    }
}

} // namespace stellar
