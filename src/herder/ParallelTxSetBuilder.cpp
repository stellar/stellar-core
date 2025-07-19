// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/ParallelTxSetBuilder.h"
#include "herder/SurgePricingUtils.h"
#include "herder/TxSetFrame.h"
#include "transactions/TransactionFrameBase.h"
#include "util/BitSet.h"
#include "util/Logging.h"

#include <unordered_set>

namespace stellar
{
namespace
{
// Configuration for parallel partitioning of transactions.
struct ParallelPartitionConfig
{
    ParallelPartitionConfig(uint32_t stageCount,
                            SorobanNetworkConfig const& sorobanCfg)
        : mStageCount(stageCount)
        , mClustersPerStage(sorobanCfg.ledgerMaxDependentTxClusters())
        , mInstructionsPerCluster(sorobanCfg.ledgerMaxInstructions() /
                                  mStageCount)
    {
        CLOG_DEBUG(Herder,
                   "ParallelPartitionConfig: stages={}, clusters/stage={}, "
                   "instructions/cluster={}, total max instructions={}",
                   mStageCount, mClustersPerStage, mInstructionsPerCluster,
                   sorobanCfg.ledgerMaxInstructions());
    }

    uint64_t
    instructionsPerStage() const
    {
        return mInstructionsPerCluster * mClustersPerStage;
    }

    uint32_t mStageCount = 0;
    uint32_t mClustersPerStage = 0;
    uint64_t mInstructionsPerCluster = 0;
};

// Internal data structure that contains only relevant transaction information
// necessary for building parallel processing stages.
struct BuilderTx
{
    size_t mId = 0;
    uint32_t mInstructions = 0;
    // Set of ids of transactions that conflict with this transaction.
    BitSet mConflictTxs;

    BuilderTx(size_t txId, TransactionFrameBase const& tx)
        : mId(txId), mInstructions(tx.sorobanResources().instructions)
    {
    }
};

// Cluster of (potentially transitively) dependent transactions.
// Transactions are considered to be dependent if the have the same key in
// their footprints and for at least one of them this key belongs to read-write
// footprint.
struct Cluster
{
    // Total number of instructions in the cluster. Since transactions are
    // dependent, these are always 'sequential' instructions.
    uint64_t mInstructions = 0;
    // Set of ids of transactions that conflict with this cluster.
    BitSet mConflictTxs;
    // Set of transaction ids in the cluster.
    BitSet mTxIds;
    // Id of the bin within a stage in which the cluster is packed.
    std::optional<size_t> mutable mBinId = std::nullopt;

    explicit Cluster(BuilderTx const& tx) : mInstructions(tx.mInstructions)
    {
        mConflictTxs.inplaceUnion(tx.mConflictTxs);
        mTxIds.set(tx.mId);
    }

    void
    merge(Cluster const& other)
    {
        mInstructions += other.mInstructions;
        mConflictTxs.inplaceUnion(other.mConflictTxs);
        mTxIds.inplaceUnion(other.mTxIds);
    }
};

// The stage of parallel processing that consists of clusters of dependent
// transactions that can be processed in parallel relative to each other
// The stage contains an arbitrary number of clusters of actually dependent
// transactions and the bin-packing of these clusters into at most
// `mConfig.mClustersPerStage` bins, i.e. into as many clusters as the network
// configuration allows.
class Stage
{
  public:
    Stage(ParallelPartitionConfig cfg) : mConfig(cfg)
    {
        mBinPacking.resize(mConfig.mClustersPerStage);
        mBinInstructions.resize(mConfig.mClustersPerStage);
    }

    // Tries to add a transaction to the stage and returns true if the
    // transaction has been added.
    bool
    tryAdd(BuilderTx const& tx)
    {
        ZoneScoped;
        // A fast-fail condition to ensure that adding the transaction won't
        // exceed the theoretical limit of instructions per stage.
        if (mInstructions + tx.mInstructions > mConfig.instructionsPerStage())
        {
            CLOG_DEBUG(Herder,
                       "Tx {} rejected from stage: would exceed instruction "
                       "limit ({} + {} > {})",
                       tx.mId, mInstructions, tx.mInstructions,
                       mConfig.instructionsPerStage());
            return false;
        }
        // First, find all clusters that conflict with the new transaction.
        auto conflictingClusters = getConflictingClusters(tx);
        CLOG_DEBUG(Herder, "Tx {} has {} conflicting clusters", tx.mId,
                   conflictingClusters.size());

        // Then, try creating new clusters by merging the conflicting clusters
        // together and adding the new transaction to the resulting cluster.
        // Note, that the new cluster is guaranteed to be added at the end of
        // `newClusters`.
        auto newClusters = createNewClusters(tx, conflictingClusters);
        // Fail fast if a new cluster will end up too large to fit into the
        // stage and thus no new clusters could be created.
        if (!newClusters)
        {
            CLOG_DEBUG(Herder,
                       "Tx {} rejected: cluster would be too large after merge",
                       tx.mId);
            return false;
        }
        // If it's possible to pack the newly-created cluster into one of the
        // bins 'in-place' without rebuilding the bin-packing, we do so.
        if (inPlaceBinPacking(*newClusters->back(), conflictingClusters))
        {
            mClusters = std::move(newClusters.value());
            mInstructions += tx.mInstructions;
            return true;
        }

        // The following is a not particularly scientific, but a useful
        // optimization.
        // The logic is as follows: in-place bin-packing is an unordered
        // first-fit heuristic with 1.7 approximation factor. Full bin
        // packing is first-fit-decreasing heuristic with 11/9 approximation,
        // which is better, but also more expensive due to full rebuild.
        // The first time we can't fit a cluster that has no conflicts with
        // first-fit heuristic, it makes sense to try re-packing all the
        // clusters with a better algorithm (thus potentially 'compacting'
        // the bins). However, after that we can say that the packing is both
        // almost at capacity and is already as compact as it gets with our
        // heuristics, so it's unlikely that if a cluster doesn't fit with
        // in-place packing, it will fit with full packing.
        // This optimization provides tremendous savings for the case when we
        // have a lot of independent transactions (say, a full tx queue with
        // 2x more transactions than we can fit into transaction set), which
        // also happens to be the worst case performance-wise. Without it we
        // might end up rebuilding the bin-packing for every single transaction
        // even though the bin-packing is already at capacity.
        // We don't do a similar optimization for the cases when there are
        // conflicts for now, as it's much less likely that all the
        // transactions would cause the cluster merge and then fail to be
        // packed (you'd need very specific set of transactions for that to
        // occur). But we can consider doing the full packing just once or a
        // few times without any additional conditions if that's ever an issue.
        if (conflictingClusters.empty())
        {
            if (mTriedCompactingBinPacking)
            {
                return false;
            }
            mTriedCompactingBinPacking = true;
        }
        // Try to recompute the bin-packing from scratch with a more efficient
        // heuristic.
        std::vector<uint64_t> newBinInstructions;
        auto newPacking = binPacking(*newClusters, newBinInstructions);
        // Even if the new cluster is below the limit, it may invalidate the
        // stage as a whole in case if we can no longer pack the clusters into
        // the required number of bins.
        if (!newPacking)
        {
            CLOG_DEBUG(Herder,
                       "Tx {} rejected: bin packing failed after rebuild",
                       tx.mId);
            return false;
        }
        mClusters = std::move(newClusters.value());
        mBinPacking = std::move(newPacking.value());
        mInstructions += tx.mInstructions;
        mBinInstructions = newBinInstructions;
        return true;
    }

    // Visit every transaction in the stage.
    // The visitor arguments are the index of the bin the transaction is packed
    // into and the index of the transaction itself.
    void
    visitAllTransactions(std::function<void(size_t, size_t)> visitor) const
    {
        for (auto const& cluster : mClusters)
        {
            size_t txId = 0;
            while (cluster->mTxIds.nextSet(txId))
            {
                visitor(cluster->mBinId.value(), txId);
                ++txId;
            }
        }
    }

  private:
    std::unordered_set<Cluster const*>
    getConflictingClusters(BuilderTx const& tx) const
    {
        std::unordered_set<Cluster const*> conflictingClusters;
        for (auto const& cluster : mClusters)
        {
            if (cluster->mConflictTxs.get(tx.mId))
            {
                conflictingClusters.insert(cluster.get());
            }
        }
        return conflictingClusters;
    }

    bool
    inPlaceBinPacking(
        Cluster const& newCluster,
        std::unordered_set<Cluster const*> const& clustersToRemove)
    {
        // Remove the clusters that were merged from their respective bins.
        for (auto const& cluster : clustersToRemove)
        {
            mBinInstructions[cluster->mBinId.value()] -= cluster->mInstructions;
            mBinPacking[cluster->mBinId.value()].inplaceDifference(
                cluster->mTxIds);
        }

        for (size_t binId = 0; binId < mConfig.mClustersPerStage; ++binId)
        {
            if (mBinInstructions[binId] + newCluster.mInstructions <=
                mConfig.mInstructionsPerCluster)
            {
                mBinInstructions[binId] += newCluster.mInstructions;
                mBinPacking[binId].inplaceUnion(newCluster.mTxIds);
                newCluster.mBinId = std::make_optional(binId);
                return true;
            }
        }
        // Revert the changes to the bins if we couldn't fit the new cluster.
        for (auto const& cluster : clustersToRemove)
        {
            mBinInstructions[cluster->mBinId.value()] += cluster->mInstructions;
            mBinPacking[cluster->mBinId.value()].inplaceUnion(cluster->mTxIds);
        }
        return false;
    }

    std::optional<std::vector<std::shared_ptr<Cluster const>>>
    createNewClusters(
        BuilderTx const& tx,
        std::unordered_set<Cluster const*> const& txConflicts) const
    {
        int64_t newInstructions = tx.mInstructions;
        for (auto const* cluster : txConflicts)
        {
            newInstructions += cluster->mInstructions;
        }

        // Fast-fail condition to ensure that the new cluster doesn't exceed
        // the instructions limit.
        if (newInstructions > mConfig.mInstructionsPerCluster)
        {
            return std::nullopt;
        }
        auto newCluster = std::make_shared<Cluster>(tx);
        for (auto const* cluster : txConflicts)
        {
            newCluster->merge(*cluster);
        }

        std::vector<std::shared_ptr<Cluster const>> newClusters;
        newClusters.reserve(mClusters.size() + 1 - txConflicts.size());
        for (auto const& cluster : mClusters)
        {
            if (txConflicts.find(cluster.get()) == txConflicts.end())
            {
                newClusters.push_back(cluster);
            }
        }
        newClusters.push_back(newCluster);
        return std::make_optional(std::move(newClusters));
    }

    // Simple bin-packing first-fit-decreasing heuristic
    // (https://en.wikipedia.org/wiki/First-fit-decreasing_bin_packing).
    // This has around 11/9 maximum approximation ratio, which probably has
    // the best complexity/performance tradeoff out of all the heuristics.
    std::optional<std::vector<BitSet>>
    binPacking(std::vector<std::shared_ptr<Cluster const>>& clusters,
               std::vector<uint64_t>& binInsns) const
    {
        // We could consider dropping the sort here in order to save some time
        // and using just the first-fit heuristic, but that also raises the
        // approximation ratio to 1.7.
        std::sort(clusters.begin(), clusters.end(),
                  [](auto const& a, auto const& b) {
                      return a->mInstructions > b->mInstructions;
                  });
        size_t const binCount = mConfig.mClustersPerStage;
        binInsns.resize(binCount);
        std::vector<BitSet> bins(binCount);
        std::vector<size_t> newBinId(clusters.size());
        // Just add every cluster into the first bin it fits into.
        for (size_t clusterId = 0; clusterId < clusters.size(); ++clusterId)
        {
            auto const& cluster = clusters[clusterId];
            bool packed = false;
            for (size_t i = 0; i < binCount; ++i)
            {
                if (binInsns[i] + cluster->mInstructions <=
                    mConfig.mInstructionsPerCluster)
                {
                    binInsns[i] += cluster->mInstructions;
                    bins[i].inplaceUnion(cluster->mTxIds);
                    newBinId[clusterId] = i;
                    packed = true;
                    break;
                }
            }
            if (!packed)
            {
                return std::nullopt;
            }
        }
        for (size_t clusterId = 0; clusterId < clusters.size(); ++clusterId)
        {
            clusters[clusterId]->mBinId =
                std::make_optional(newBinId[clusterId]);
        }
        return std::make_optional(bins);
    }
    // The `Cluster`s in `mClusters` are groups of transactions that have
    // (transitive) data dependencies between one another. If there is a data
    // dependency between a tx in cluster A and a tx in cluster B, the clusters
    // A and B are merged. A cluster is just a `BitSet` of ids of transactions
    // that belong to it and a `BitSet` of ids of transactions that conflict
    // with transactions inside the cluster. Each of the `BitSet`s grows as
    // clusters are built from transactions and merged with other clusters.
    //
    // Looked at another way: two clusters that _aren't_ merged by the end of
    // the process of forming clusters _are_ data-independent and _could_
    // potentially run in parallel.
    std::vector<std::shared_ptr<Cluster const>> mClusters;
    // The clusters formed by data dependency merging may, however,
    // significantly outnumber the maximum _allowed_ amount of parallelism in
    // the stage -- a number called `ledgerMaxDependentTxClusters` in CAP-0063
    // -- for example we might have a txset with hundreds of independent,
    // potentially-parallel clusters, but be running on a network that only
    // guarantees (at least) 8-way parallel execution. In this case we pack the
    // hundreds of clusters into 8 "bins", using a bin-packing heuristic.
    //
    // The bins are represented as `BitSet`s of transaction ids, just like the
    // transaction id sets in `Cluster`s, and in fact when forming an XDR
    // `GeneralizedTransactionSet` a "bin" here is what becomes a single
    // `DependentTxCluster`. In a sense the bins are just "artificial
    // super-clusters" that do not arise from any logical data-dependence, just
    // the requirement to arrive at a smaller number of final clusters to
    // schedule in parallel.
    //
    // One might imagine that we could just present the fine-grained clusters
    // caused by logical data dependency "as-is", and allow the network to run
    // them with "as much parallelism as it can", but this runs the risk of an
    // underpowered node scheduling the clusters on too _few_ threads, and
    // exceeding its close-time target. By establishing a _minimum_ number of
    // threads that all nodes _must_ have, and running bin-packing against that
    // minimum assumption, we can form txsets into binned clusters each small
    // enough to run in the close-time target on the guaranteed parallelism.
    std::vector<BitSet> mBinPacking;
    std::vector<uint64_t> mBinInstructions;
    int64_t mInstructions = 0;
    ParallelPartitionConfig mConfig;
    bool mTriedCompactingBinPacking = false;
};

struct ParallelPhaseBuildResult
{
    TxStageFrameList mStages;
    std::vector<bool> mHadTxNotFittingLane;
    int64_t mTotalInclusionFee = 0;
};

ParallelPhaseBuildResult
buildSurgePricedParallelSorobanPhaseWithStageCount(
    SurgePricingPriorityQueue queue,
    std::unordered_map<TransactionFrameBaseConstPtr, BuilderTx const*> const&
        builderTxForTx,
    TxFrameList const& txFrames, uint32_t stageCount,
    SorobanNetworkConfig const& sorobanCfg,
    std::shared_ptr<SurgePricingLaneConfig> laneConfig, uint32_t ledgerVersion)
{
    ZoneScoped;
    ParallelPartitionConfig partitionCfg(stageCount, sorobanCfg);

    std::vector<Stage> stages(partitionCfg.mStageCount, partitionCfg);

    // Visit the transactions in the surge pricing queue and try to add them to
    // at least one of the stages.
    size_t processedCount = 0;
    size_t rejectedCount = 0;
    auto visitor = [&stages, &builderTxForTx, &processedCount,
                    &rejectedCount](TransactionFrameBaseConstPtr const& tx) {
        bool added = false;
        auto builderTxIt = builderTxForTx.find(tx);
        releaseAssert(builderTxIt != builderTxForTx.end());

        size_t stageNum = 0;
        for (auto& stage : stages)
        {
            if (stage.tryAdd(*builderTxIt->second))
            {
                added = true;
                CLOG_DEBUG(Herder, "Transaction {} added to stage {}",
                           builderTxIt->second->mId, stageNum);
                break;
            }
            stageNum++;
        }
        if (added)
        {
            processedCount++;
            return SurgePricingPriorityQueue::VisitTxResult::PROCESSED;
        }
        // If a transaction didn't fit into any of the stages, we consider it
        // to have been excluded due to resource limits and thus notify the
        // surge pricing queue that surge pricing should be triggered (
        // REJECTED imitates the behavior for exceeding the resource limit
        // within the queue itself).
        rejectedCount++;
        CLOG_DEBUG(Herder,
                   "Transaction {} rejected - couldn't fit in any stage",
                   builderTxIt->second->mId);
        return SurgePricingPriorityQueue::VisitTxResult::REJECTED;
    };

    ParallelPhaseBuildResult result;
    std::vector<Resource> laneLeftUntilLimitUnused;
    queue.popTopTxs(/* allowGaps */ true, visitor, laneLeftUntilLimitUnused,
                    result.mHadTxNotFittingLane, ledgerVersion);

    CLOG_DEBUG(
        Herder,
        "Stage building complete: {} processed, {} rejected, stage count {}",
        processedCount, rejectedCount, stageCount);

    // There is only a single fee lane for Soroban, so there is only a single
    // flag that indicates whether there was a transaction that didn't fit into
    // lane (and thus all transactions are surge priced at once).
    releaseAssert(result.mHadTxNotFittingLane.size() == 1);

    // At this point the stages have been filled with transactions and we just
    // need to place the full transactions into the respective stages/clusters.
    result.mStages.reserve(stages.size());
    int64_t& totalInclusionFee = result.mTotalInclusionFee;
    for (auto const& stage : stages)
    {
        auto& resStage = result.mStages.emplace_back();
        resStage.reserve(partitionCfg.mClustersPerStage);

        std::unordered_map<size_t, size_t> clusterIdToStageCluster;

        stage.visitAllTransactions(
            [&resStage, &txFrames, &clusterIdToStageCluster,
             &totalInclusionFee](size_t clusterId, size_t txId) {
                auto it = clusterIdToStageCluster.find(clusterId);
                if (it == clusterIdToStageCluster.end())
                {
                    it = clusterIdToStageCluster
                             .emplace(clusterId, resStage.size())
                             .first;
                    resStage.emplace_back();
                }
                totalInclusionFee += txFrames[txId]->getInclusionFee();
                resStage[it->second].push_back(txFrames[txId]);
            });
        // Algorithm ensures that clusters are populated from first to last and
        // no empty clusters are generated.
        for (auto const& cluster : resStage)
        {
            releaseAssert(!cluster.empty());
        }
        CLOG_DEBUG(Herder, "Stage {} has {} clusters", stages.size() - 1,
                   resStage.size());
    }
    // Ensure we don't return any empty stages, which is prohibited by the
    // protocol. The algorithm builds the stages such that the stages are
    // populated from first to last.
    size_t emptyStagesRemoved = 0;
    while (!result.mStages.empty() && result.mStages.back().empty())
    {
        result.mStages.pop_back();
        emptyStagesRemoved++;
    }

    CLOG_DEBUG(Herder,
               "Final result: {} stages, {} empty stages removed, total fee {}",
               result.mStages.size(), emptyStagesRemoved,
               result.mTotalInclusionFee);

    for (auto const& stage : result.mStages)
    {
        releaseAssert(!stage.empty());
    }

    return result;
}

} // namespace

TxStageFrameList
buildSurgePricedParallelSorobanPhase(
    TxFrameList const& txFrames, Config const& cfg,
    SorobanNetworkConfig const& sorobanCfg,
    std::shared_ptr<SurgePricingLaneConfig> laneConfig,
    std::vector<bool>& hadTxNotFittingLane, uint32_t ledgerVersion)
{
    ZoneScoped;
    CLOG_DEBUG(Herder, "Building parallel Soroban phase with {} transactions",
               txFrames.size());

    // We prefer the transaction sets that are well utilized, but we also want
    // to lower the stage count when possible. Thus we will nominate a tx set
    // that has the lowest amount of stages while still being within
    // MAX_INCLUSION_FEE_TOLERANCE_FOR_STAGE_COUNT from the maximum total
    // inclusion fee (a proxy for the transaction set utilization).
    double const MAX_INCLUSION_FEE_TOLERANCE_FOR_STAGE_COUNT = 0.999;

    // Simplify the transactions to the minimum necessary amount of data.
    std::unordered_map<TransactionFrameBaseConstPtr, BuilderTx const*>
        builderTxForTx;
    std::vector<std::unique_ptr<BuilderTx>> builderTxs;
    builderTxs.reserve(txFrames.size());
    for (size_t i = 0; i < txFrames.size(); ++i)
    {
        auto const& txFrame = txFrames[i];
        builderTxs.emplace_back(std::make_unique<BuilderTx>(i, *txFrame));
        builderTxForTx.emplace(txFrame, builderTxs.back().get());
        CLOG_DEBUG(Herder, "Transaction {} has {} instructions", i,
                   txFrame->sorobanResources().instructions);
    }

    // Before trying to include any transactions, find all the pairs of the
    // conflicting transactions and mark the conflicts in the builderTxs.
    //
    // In order to find the conflicts, we build the maps from the footprint
    // keys to transactions, then mark the conflicts between the transactions
    // that share RW key, or between the transactions that share RO and RW key.
    //
    // The approach here is optimized towards the low number of conflicts,
    // specifically when there are no conflicts at all, the complexity is just
    // O(total_footprint_entry_count). The worst case is roughly
    // O(max_tx_footprint_size * transaction_count ^ 2), which is equivalent
    // to the complexity of the straightforward approach of iterating over all
    // the transaction pairs.
    //
    // This also has the further optimization potential: we could populate the
    // key maps and even the conflicting transactions eagerly in tx queue, thus
    // amortizing the costs across the whole ledger duration.
    UnorderedMap<LedgerKey, std::vector<size_t>> txsWithRoKey;
    UnorderedMap<LedgerKey, std::vector<size_t>> txsWithRwKey;
    for (size_t i = 0; i < txFrames.size(); ++i)
    {
        auto const& txFrame = txFrames[i];
        auto const& footprint = txFrame->sorobanResources().footprint;
        for (auto const& key : footprint.readOnly)
        {
            txsWithRoKey[key].push_back(i);
        }
        for (auto const& key : footprint.readWrite)
        {
            txsWithRwKey[key].push_back(i);
        }
    }

    CLOG_DEBUG(Herder,
               "Found {} RO keys and {} RW keys across all transactions",
               txsWithRoKey.size(), txsWithRwKey.size());

    size_t totalConflicts = 0;
    for (auto const& [key, rwTxIds] : txsWithRwKey)
    {
        // RW-RW conflicts
        for (size_t i = 0; i < rwTxIds.size(); ++i)
        {
            for (size_t j = i + 1; j < rwTxIds.size(); ++j)
            {
                builderTxs[rwTxIds[i]]->mConflictTxs.set(rwTxIds[j]);
                builderTxs[rwTxIds[j]]->mConflictTxs.set(rwTxIds[i]);
                totalConflicts += 1;
            }
        }
        // RO-RW conflicts
        auto roIt = txsWithRoKey.find(key);
        if (roIt != txsWithRoKey.end())
        {
            auto const& roTxIds = roIt->second;
            for (size_t i = 0; i < roTxIds.size(); ++i)
            {
                for (size_t j = 0; j < rwTxIds.size(); ++j)
                {
                    builderTxs[roTxIds[i]]->mConflictTxs.set(rwTxIds[j]);
                    builderTxs[rwTxIds[j]]->mConflictTxs.set(roTxIds[i]);
                    totalConflicts += 1;
                }
            }
        }
    }

    CLOG_DEBUG(Herder, "Found {} total conflicts between transactions",
               totalConflicts);

    // Process the transactions in the surge pricing (decreasing fee) order.
    // This also automatically ensures that the resource limits are respected
    // for all the dimensions besides instructions.
    SurgePricingPriorityQueue queue(
        /* isHighestPriority */ true, laneConfig,
        stellar::rand_uniform<size_t>(0, std::numeric_limits<size_t>::max()));

    CLOG_DEBUG(Herder, "Adding {} transactions to surge pricing queue",
               txFrames.size());

    for (auto const& tx : txFrames)
    {
        queue.add(tx, ledgerVersion);
    }

    // Create a worker thread for each stage count.
    std::vector<std::thread> threads;
    uint32_t stageCountOptions = cfg.SOROBAN_PHASE_MAX_STAGE_COUNT -
                                 cfg.SOROBAN_PHASE_MIN_STAGE_COUNT + 1;
    std::vector<ParallelPhaseBuildResult> results(stageCountOptions);

    CLOG_DEBUG(Herder, "Creating {} threads for stage counts {} to {}",
               stageCountOptions, cfg.SOROBAN_PHASE_MIN_STAGE_COUNT,
               cfg.SOROBAN_PHASE_MAX_STAGE_COUNT);

    for (uint32_t stageCount = cfg.SOROBAN_PHASE_MIN_STAGE_COUNT;
         stageCount <= cfg.SOROBAN_PHASE_MAX_STAGE_COUNT; ++stageCount)
    {
        size_t resultIndex = stageCount - cfg.SOROBAN_PHASE_MIN_STAGE_COUNT;
        threads.emplace_back([queue, &builderTxForTx, txFrames, stageCount,
                              sorobanCfg, laneConfig, resultIndex, &results,
                              ledgerVersion]() {
            results.at(resultIndex) =
                buildSurgePricedParallelSorobanPhaseWithStageCount(
                    std::move(queue), builderTxForTx, txFrames, stageCount,
                    sorobanCfg, laneConfig, ledgerVersion);
        });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }

    int64_t maxTotalInclusionFee = 0;
    for (auto const& result : results)
    {
        maxTotalInclusionFee =
            std::max(maxTotalInclusionFee, result.mTotalInclusionFee);
    }
    maxTotalInclusionFee *= MAX_INCLUSION_FEE_TOLERANCE_FOR_STAGE_COUNT;

    CLOG_DEBUG(Herder, "Max total inclusion fee: {}, tolerance threshold: {}",
               maxTotalInclusionFee /
                   MAX_INCLUSION_FEE_TOLERANCE_FOR_STAGE_COUNT,
               maxTotalInclusionFee);

    std::optional<size_t> bestResultIndex = std::nullopt;
    for (size_t i = 0; i < results.size(); ++i)
    {
        CLOG_DEBUG(Herder,
                   "Parallel Soroban tx set nomination: {} stages => {} total "
                   "inclusion fee",
                   results[i].mStages.size(), results[i].mTotalInclusionFee);
        if (results[i].mTotalInclusionFee < maxTotalInclusionFee)
        {
            CLOG_DEBUG(Herder, "Skipping result {} - fee {} below threshold {}",
                       i, results[i].mTotalInclusionFee, maxTotalInclusionFee);
            continue;
        }
        if (!bestResultIndex ||
            results[i].mStages.size() <
                results[bestResultIndex.value()].mStages.size())
        {
            CLOG_DEBUG(Herder, "New best result: index {} with {} stages", i,
                       results[i].mStages.size());
            bestResultIndex = std::make_optional(i);
        }
    }

    if (!bestResultIndex.has_value())
    {
        CLOG_ERROR(Herder, "No valid parallel phase result found!");
        releaseAssert(false);
    }

    auto& bestResult = results[bestResultIndex.value()];
    CLOG_DEBUG(Herder, "Selected result {} with {} stages and {} total fee",
               bestResultIndex.value(), bestResult.mStages.size(),
               bestResult.mTotalInclusionFee);

    hadTxNotFittingLane = std::move(bestResult.mHadTxNotFittingLane);
    return std::move(bestResult.mStages);
}

#ifdef BUILD_TESTS

// This next section provides a test-oriented "simple" builder that does not
// attempt bin-packing, does not use the txqueue, and will not drop or reorder
// dependent txs across stages: if dependent txs i < j are in the input, the
// stage structure that goes out should still have i executing before j, either
// in a single cluster or across stages. This exists entirely for the parallel
// pre-execution code in LedgerManagerImpl.cpp that synthesizes parallel
// schedules to match sequential ones online during testing.
namespace
{
// Represents a cluster of transaction indices that must execute sequentially
struct IndexCluster
{
    BitSet mTxIds;     // Set of transaction IDs in this cluster
    BitSet mConflicts; // Union of all conflict sets of txs in cluster
};

// Core algorithm that works with indices and BitSets
std::vector<std::vector<std::vector<size_t>>>
buildSimpleParallelStagesFromIndices(std::vector<BitSet> const& conflictSets,
                                     uint32_t clustersPerStage,
                                     uint32_t targetStageCount)
{

    if (conflictSets.empty() || targetStageCount == 0 || clustersPerStage == 0)
    {
        return {};
    }

    std::vector<std::vector<std::vector<size_t>>> result;
    std::vector<size_t> buffer;
    std::vector<size_t> nextBuffer;
    size_t inputIndex = 0;
    size_t inputEnd = conflictSets.size();

    // Calculate target transactions per stage
    size_t targetTxsPerStage = conflictSets.size() / targetStageCount;
    if (targetTxsPerStage == 0)
    {
        targetTxsPerStage = 1;
    }

    // Build stages until all transactions are processed
    while (inputIndex < inputEnd || !buffer.empty())
    {
        std::vector<IndexCluster> stageClusters;
        size_t txsProcessedInStage = 0;

        // Helper to check invariant: no two clusters should conflict
        auto checkNonConflictingInvariant = [&]() {
            for (size_t i = 0; i < stageClusters.size(); ++i)
            {
                for (size_t j = i + 1; j < stageClusters.size(); ++j)
                {
                    if (stageClusters[i].mTxIds.intersectionCount(
                            stageClusters[j].mConflicts) > 0 ||
                        stageClusters[j].mTxIds.intersectionCount(
                            stageClusters[i].mConflicts) > 0)
                    {
                        throw std::runtime_error(
                            "Invariant violated: clusters conflict");
                    }
                }
            }
        };

        // Process a transaction
        auto processTx = [&](size_t txId) -> bool {
            CLOG_DEBUG(Herder, "processing txid {}", txId);
            // Find all clusters that this tx conflicts with
            std::vector<size_t> conflictingClusters;
            for (size_t i = 0; i < stageClusters.size(); ++i)
            {
                auto& cluster = stageClusters[i];
                // Check if this tx conflicts with the cluster
                if (cluster.mTxIds.intersectionCount(conflictSets[txId]) > 0 ||
                    cluster.mConflicts.get(txId))
                {
                    CLOG_DEBUG(Herder, "txid {} conflicts with cluster {}",
                               txId, i);
                    conflictingClusters.push_back(i);
                }
            }

            // If no conflicts and we have room for a new cluster
            if (conflictingClusters.empty() &&
                stageClusters.size() < clustersPerStage)
            {
                CLOG_DEBUG(Herder, "forming new cluster with txid {}", txId);
                IndexCluster newCluster;
                newCluster.mTxIds.set(txId);
                newCluster.mConflicts = conflictSets[txId];
                stageClusters.push_back(std::move(newCluster));
                txsProcessedInStage++;
                checkNonConflictingInvariant();
                return true;
            }

            // If we have conflicts, merge all conflicting clusters
            if (!conflictingClusters.empty())
            {
                CLOG_DEBUG(Herder, "merging {} conflicting clusters",
                           conflictingClusters.size());
                // Build the merged cluster
                IndexCluster mergedCluster;
                for (size_t clusterIdx : conflictingClusters)
                {
                    auto& cluster = stageClusters[clusterIdx];
                    mergedCluster.mTxIds.inplaceUnion(cluster.mTxIds);
                    mergedCluster.mConflicts.inplaceUnion(cluster.mConflicts);
                }

                // Add new transaction at the end
                mergedCluster.mTxIds.set(txId);
                mergedCluster.mConflicts.inplaceUnion(conflictSets[txId]);

                // Remove all conflicting clusters (in reverse order)
                for (auto it = conflictingClusters.rbegin();
                     it != conflictingClusters.rend(); ++it)
                {
                    stageClusters.erase(stageClusters.begin() + *it);
                }

                // Add the merged cluster
                stageClusters.push_back(std::move(mergedCluster));

                txsProcessedInStage++;

                checkNonConflictingInvariant();
                return true;
            }

            // Couldn't place in stage, buffer for next stage
            CLOG_DEBUG(Herder,
                       "unable to place txid {}, buffering to next stage",
                       txId);
            return false;
        };

        // Process _incoming_ buffered txs from previous stage first
        CLOG_DEBUG(Herder, "incoming buffered txs: {}", buffer.size());
        for (size_t txId : buffer)
        {
            if (!processTx(txId))
            {
                nextBuffer.push_back(txId);
            }
        }

        // Assuming we drained the incoming buffer (which should almost always
        // happen) process new transactions from input
        if (nextBuffer.empty())
        {
            CLOG_DEBUG(
                Herder,
                "incoming buffered txs all assigned, proceeding with input");
            while (inputIndex < inputEnd &&
                   txsProcessedInStage < targetTxsPerStage)
            {
                // Try to process the transaction
                if (!processTx(inputIndex))
                {
                    nextBuffer.push_back(inputIndex);
                }
                inputIndex++;
            }
        }

        // Convert stage clusters to result format
        if (!stageClusters.empty())
        {
            std::vector<std::vector<size_t>> stage;
            for (auto& cluster : stageClusters)
            {
                if (!cluster.mTxIds.empty())
                {
                    std::vector<size_t> clu;
                    for (size_t i = 0; cluster.mTxIds.nextSet(i); ++i)
                    {
                        clu.push_back(i);
                    }
                    stage.push_back(clu);
                }
            }
            if (!stage.empty())
            {
                CLOG_DEBUG(Herder, "formed stage with {} clusters",
                           stage.size());
                result.push_back(std::move(stage));
            }
        }

        // Swap buffers for next iteration
        buffer = std::move(nextBuffer);
        nextBuffer.clear();
    }

    return result;
}
} // namespace

// Test function that builds a simple TxStageFrameList with fixed parallelism
// This function ignores resource limits and surge pricing
TxStageFrameList
buildSimpleParallelTxStages(TxFrameList const& txFrames,
                            uint32_t clustersPerStage,
                            uint32_t targetStageCount)
{
    ZoneScoped;
    CLOG_DEBUG(Herder,
               "Building simple parallel stages: {} txs, target {} stages, {} "
               "clusters/stage",
               txFrames.size(), targetStageCount, clustersPerStage);

    if (txFrames.empty() || targetStageCount == 0 || clustersPerStage == 0)
    {
        return {};
    }

    // Build conflict map similar to the main function
    UnorderedMap<LedgerKey, std::vector<size_t>> txsWithRoKey;
    UnorderedMap<LedgerKey, std::vector<size_t>> txsWithRwKey;
    std::vector<BitSet> conflictSets(txFrames.size());

    for (size_t i = 0; i < txFrames.size(); ++i)
    {
        auto const& txFrame = txFrames[i];
        auto const& footprint = txFrame->sorobanResources().footprint;
        for (auto const& key : footprint.readOnly)
        {
            txsWithRoKey[key].push_back(i);
        }
        for (auto const& key : footprint.readWrite)
        {
            txsWithRwKey[key].push_back(i);
        }
    }

    // Mark conflicts
    for (auto const& [key, rwTxIds] : txsWithRwKey)
    {
        // RW-RW conflicts
        for (size_t i = 0; i < rwTxIds.size(); ++i)
        {
            for (size_t j = i + 1; j < rwTxIds.size(); ++j)
            {
                conflictSets[rwTxIds[i]].set(rwTxIds[j]);
                conflictSets[rwTxIds[j]].set(rwTxIds[i]);
            }
        }
        // RO-RW conflicts
        auto roIt = txsWithRoKey.find(key);
        if (roIt != txsWithRoKey.end())
        {
            auto const& roTxIds = roIt->second;
            for (size_t i = 0; i < roTxIds.size(); ++i)
            {
                for (size_t j = 0; j < rwTxIds.size(); ++j)
                {
                    conflictSets[roTxIds[i]].set(rwTxIds[j]);
                    conflictSets[rwTxIds[j]].set(roTxIds[i]);
                }
            }
        }
    }

    // Use the core algorithm
    auto indexStages = buildSimpleParallelStagesFromIndices(
        conflictSets, clustersPerStage, targetStageCount);

    // Convert index stages to TxStageFrameList
    TxStageFrameList result;
    for (auto const& indexStage : indexStages)
    {
        TxStageFrame stage;
        for (auto const& indexCluster : indexStage)
        {
            TxFrameList cluster;
            for (size_t txId : indexCluster)
            {
                cluster.push_back(txFrames[txId]);
            }
            stage.push_back(std::move(cluster));
        }
        result.push_back(std::move(stage));
    }

    CLOG_DEBUG(Herder, "Built {} stages from {} transactions", result.size(),
               txFrames.size());

    return result;
}

// Export the core algorithm for testing
std::vector<std::vector<std::vector<size_t>>>
testBuildSimpleParallelStagesFromIndices(
    std::vector<BitSet> const& conflictSets, uint32_t clustersPerStage,
    uint32_t targetStageCount)
{
    return buildSimpleParallelStagesFromIndices(conflictSets, clustersPerStage,
                                                targetStageCount);
}

#endif // BUILD_TESTS

} // namespace stellar
