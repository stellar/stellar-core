// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/ParallelTxSetBuilder.h"
#include "herder/SurgePricingUtils.h"
#include "herder/TxSetFrame.h"
#include "transactions/TransactionFrameBase.h"
#include "util/BitSet.h"

#include <unordered_set>

namespace stellar
{
namespace
{
// Configuration for parallel partitioning of transactions.
struct ParallelPartitionConfig
{
    ParallelPartitionConfig(Config const& cfg,
                            SorobanNetworkConfig const& sorobanCfg)
        : mStageCount(
              std::max(cfg.SOROBAN_PHASE_STAGE_COUNT, static_cast<uint32_t>(1)))
        , mClustersPerStage(sorobanCfg.ledgerMaxDependentTxClusters())
        , mInstructionsPerCluster(sorobanCfg.ledgerMaxInstructions() /
                                  mStageCount)
    {
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
            return false;
        }
        // First, find all clusters that conflict with the new transaction.
        auto conflictingClusters = getConflictingClusters(tx);

        // Then, try creating new clusters by merging the conflicting clusters
        // together and adding the new transaction to the resulting cluster.
        // Note, that the new cluster is guaranteed to be added at the end of
        // `newClusters`.
        auto newClusters = createNewClusters(tx, conflictingClusters);
        // Fail fast if a new cluster will end up too large to fit into the
        // stage and thus no new clusters could be created.
        if (!newClusters)
        {
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
        // Otherwise, we need try to recompute the bin-packing from scratch.
        std::vector<uint64_t> newBinInstructions;
        auto newPacking = binPacking(*newClusters, newBinInstructions);
        // Even if the new cluster is below the limit, it may invalidate the
        // stage as a whole in case if we can no longer pack the clusters into
        // the required number of bins.
        if (!newPacking)
        {
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
};

} // namespace

TxStageFrameList
buildSurgePricedParallelSorobanPhase(
    TxFrameList const& txFrames, Config const& cfg,
    SorobanNetworkConfig const& sorobanCfg,
    std::shared_ptr<SurgePricingLaneConfig> laneConfig,
    std::vector<bool>& hadTxNotFittingLane)
{
    ZoneScoped;
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

    for (auto const& [key, rwTxIds] : txsWithRwKey)
    {
        // RW-RW conflicts
        for (size_t i = 0; i < rwTxIds.size(); ++i)
        {
            for (size_t j = i + 1; j < rwTxIds.size(); ++j)
            {
                builderTxs[rwTxIds[i]]->mConflictTxs.set(rwTxIds[j]);
                builderTxs[rwTxIds[j]]->mConflictTxs.set(rwTxIds[i]);
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
                }
            }
        }
    }

    // Process the transactions in the surge pricing (decreasing fee) order.
    // This also automatically ensures that the resource limits are respected
    // for all the dimensions besides instructions.
    SurgePricingPriorityQueue queue(
        /* isHighestPriority */ true, laneConfig,
        stellar::rand_uniform<size_t>(0, std::numeric_limits<size_t>::max()));
    for (auto const& tx : txFrames)
    {
        queue.add(tx);
    }

    ParallelPartitionConfig partitionCfg(cfg, sorobanCfg);
    std::vector<Stage> stages(partitionCfg.mStageCount, partitionCfg);

    // Visit the transactions in the surge pricing queue and try to add them to
    // at least one of the stages.
    auto visitor = [&stages,
                    &builderTxForTx](TransactionFrameBaseConstPtr const& tx) {
        bool added = false;
        auto builderTxIt = builderTxForTx.find(tx);
        releaseAssert(builderTxIt != builderTxForTx.end());
        for (auto& stage : stages)
        {
            if (stage.tryAdd(*builderTxIt->second))
            {
                added = true;
                break;
            }
        }
        if (added)
        {
            return SurgePricingPriorityQueue::VisitTxResult::PROCESSED;
        }
        // If a transaction didn't fit into any of the stages, we consider it
        // to have been excluded due to resource limits and thus notify the
        // surge pricing queue that surge pricing should be triggered (
        // REJECTED imitates the behavior for exceeding the resource limit
        // within the queue itself).
        return SurgePricingPriorityQueue::VisitTxResult::REJECTED;
    };

    std::vector<Resource> laneLeftUntilLimitUnused;
    queue.popTopTxs(/* allowGaps */ true, visitor, laneLeftUntilLimitUnused,
                    hadTxNotFittingLane);
    // There is only a single fee lane for Soroban, so there is only a single
    // flag that indicates whether there was a transaction that didn't fit into
    // lane (and thus all transactions are surge priced at once).
    releaseAssert(hadTxNotFittingLane.size() == 1);

    // At this point the stages have been filled with transactions and we just
    // need to place the full transactions into the respective stages/clusters.
    TxStageFrameList resStages;
    resStages.reserve(stages.size());
    for (auto const& stage : stages)
    {
        auto& resStage = resStages.emplace_back();
        resStage.reserve(partitionCfg.mClustersPerStage);

        std::unordered_map<size_t, size_t> clusterIdToStageCluster;

        stage.visitAllTransactions([&resStage, &txFrames,
                                    &clusterIdToStageCluster](size_t clusterId,
                                                              size_t txId) {
            auto it = clusterIdToStageCluster.find(clusterId);
            if (it == clusterIdToStageCluster.end())
            {
                it = clusterIdToStageCluster.emplace(clusterId, resStage.size())
                         .first;
                resStage.emplace_back();
            }
            resStage[it->second].push_back(txFrames[txId]);
        });
        // Algorithm ensures that clusters are populated from first to last and
        // no empty clusters are generated.
        for (auto const& cluster : resStage)
        {
            releaseAssert(!cluster.empty());
        }
    }
    // Ensure we don't return any empty stages, which is prohibited by the
    // protocol. The algorithm builds the stages such that the stages are
    // populated from first to last.
    while (!resStages.empty() && resStages.back().empty())
    {
        resStages.pop_back();
    }
    for (auto const& stage : resStages)
    {
        releaseAssert(!stage.empty());
    }

    return resStages;
}

} // namespace stellar
