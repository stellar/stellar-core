// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/ParallelTxSetBuilder.h"
#include "herder/SurgePricingUtils.h"
#include "herder/TxSetFrame.h"
#include "transactions/TransactionFrameBase.h"
#include "util/BitSet.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <numeric>
#include <thread>
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
    Stage(Stage const&) = delete;
    Stage& operator=(Stage const&) = delete;

    Stage(Stage&&) = default;
    Stage& operator=(Stage&&) = default;

    Stage(ParallelPartitionConfig cfg, size_t txCount)
        : mConfig(cfg), mTxToCluster(txCount, nullptr)
    {
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

        // Check if the merged cluster would exceed the instruction limit.
        uint64_t mergedInstructions = tx.mInstructions;
        for (auto const* cluster : conflictingClusters)
        {
            mergedInstructions += cluster->mInstructions;
        }
        if (mergedInstructions > mConfig.mInstructionsPerCluster)
        {
            return false;
        }

        // Fast path for transactions that don't conflict with any existing
        // cluster: no clusters need to be merged, so the new singleton
        // cluster either fits into one of the bins in-place, or the stage is
        // at capacity (modulo the one-off full repacking attempt below).
        // This makes rejecting a transaction from a full stage an O(1)
        // operation (a single check against the least loaded bin), which
        // matters a lot when there are many more candidate transactions than
        // the stage can fit.
        if (conflictingClusters.empty())
        {
            if (mMinBinInstructions + tx.mInstructions <=
                mConfig.mInstructionsPerCluster)
            {
                auto newCluster = std::make_unique<Cluster>(tx);
                for (size_t binId = 0; binId < mConfig.mClustersPerStage;
                     ++binId)
                {
                    if (mBinInstructions[binId] + tx.mInstructions <=
                        mConfig.mInstructionsPerCluster)
                    {
                        mBinInstructions[binId] += tx.mInstructions;
                        newCluster->mBinId = std::make_optional(binId);
                        break;
                    }
                }
                releaseAssert(newCluster->mBinId.has_value());
                mClusters.push_back(std::move(newCluster));
                finalizeAddedCluster(tx, *mClusters.back());
                return true;
            }

            // The following is a not particularly scientific, but a useful
            // optimization.
            // The logic is as follows: in-place bin-packing is an unordered
            // first-fit heuristic with 1.7 approximation factor. Full bin
            // packing is first-fit-decreasing heuristic with 11/9
            // approximation, which is better, but also more expensive due to
            // full rebuild.
            // The first time we can't fit a cluster that has no conflicts
            // with first-fit heuristic, it makes sense to try re-packing all
            // the clusters with a better algorithm (thus potentially
            // 'compacting' the bins). However, after that we can say that the
            // packing is both almost at capacity and is already as compact as
            // it gets with our heuristics, so it's unlikely that if a cluster
            // doesn't fit with in-place packing, it will fit with full
            // packing.
            // This optimization provides tremendous savings for the case when
            // we have a lot of independent transactions (say, a full tx queue
            // with 2x more transactions than we can fit into transaction
            // set), which also happens to be the worst case performance-wise.
            // Without it we might end up rebuilding the bin-packing for every
            // single transaction even though the bin-packing is already at
            // capacity.
            // We don't do a similar optimization for the cases when there are
            // conflicts for now, as it's much less likely that all the
            // transactions would cause the cluster merge and then fail to be
            // packed (you'd need very specific set of transactions for that
            // to occur). But we can consider doing the full packing just once
            // or a few times without any additional conditions if that's ever
            // an issue.
            if (mTriedCompactingBinPacking)
            {
                return false;
            }
            mTriedCompactingBinPacking = true;
            mClusters.push_back(std::make_unique<Cluster>(tx));
            auto* addedCluster = mClusters.back().get();
            // Try to recompute the bin-packing from scratch with a more
            // efficient heuristic. binPacking() sorts mClusters in-place.
            std::vector<uint64_t> newBinInstructions;
            if (!binPacking(mClusters, newBinInstructions))
            {
                std::vector<std::unique_ptr<Cluster const>> noSavedClusters;
                rollbackClusters(addedCluster, noSavedClusters);
                return false;
            }
            mBinInstructions = newBinInstructions;
            finalizeAddedCluster(tx, *addedCluster);
            return true;
        }

        // Create the merged cluster from the new transaction and all
        // conflicting clusters.
        auto newCluster = std::make_unique<Cluster>(tx);
        for (auto const* cluster : conflictingClusters)
        {
            newCluster->merge(*cluster);
        }

        // Mutate mClusters in-place: remove conflicting clusters (saving
        // them for potential rollback) and append the new merged cluster.
        std::vector<std::unique_ptr<Cluster const>> savedClusters;
        savedClusters.reserve(conflictingClusters.size());
        removeConflictingClusters(conflictingClusters, savedClusters);
        mClusters.push_back(std::move(newCluster));

        // If it's possible to pack the newly-created cluster into one of the
        // bins 'in-place' without rebuilding the bin-packing, we do so.
        auto* addedCluster = mClusters.back().get();
        if (inPlaceBinPacking(*addedCluster, conflictingClusters))
        {
            finalizeAddedCluster(tx, *addedCluster);
            return true;
        }

        // Try to recompute the bin-packing from scratch with a more efficient
        // heuristic. binPacking() sorts mClusters in-place.
        std::vector<uint64_t> newBinInstructions;
        // Even if the new cluster is below the limit, it may invalidate the
        // stage as a whole in case if we can no longer pack the clusters into
        // the required number of bins.
        if (!binPacking(mClusters, newBinInstructions))
        {
            rollbackClusters(addedCluster, savedClusters);
            return false;
        }
        mBinInstructions = newBinInstructions;
        finalizeAddedCluster(tx, *addedCluster);
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
        // Fast path: if the tx's id is not in any cluster's conflict set,
        // there are no conflicting clusters.
        if (!mAllConflictTxs.get(tx.mId))
        {
            return {};
        }
        // O(K) lookup: iterate the conflict tx ids and find their
        // clusters via the tx-to-cluster mapping, instead of scanning
        // all clusters (which would be O(C)).
        std::unordered_set<Cluster const*> conflictingClusters;
        size_t conflictTxId = 0;
        while (tx.mConflictTxs.nextSet(conflictTxId))
        {
            auto const* cluster = mTxToCluster[conflictTxId];
            if (cluster != nullptr)
            {
                conflictingClusters.insert(cluster);
            }
            ++conflictTxId;
        }
        return conflictingClusters;
    }

    void
    updateTxToCluster(Cluster const& cluster)
    {
        auto* clusterPtr = &cluster;
        size_t txId = 0;
        while (cluster.mTxIds.nextSet(txId))
        {
            mTxToCluster[txId] = clusterPtr;
            ++txId;
        }
    }

    // Common bookkeeping for every successful cluster addition.
    void
    finalizeAddedCluster(BuilderTx const& tx, Cluster const& addedCluster)
    {
        mInstructions += tx.mInstructions;
        // Update the global conflict mask so future lookups can
        // fast-path when a tx has no conflicts with any cluster.
        mAllConflictTxs.inplaceUnion(tx.mConflictTxs);
        updateTxToCluster(addedCluster);
        mMinBinInstructions = *std::min_element(mBinInstructions.begin(),
                                                mBinInstructions.end());
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
        }

        for (size_t binId = 0; binId < mConfig.mClustersPerStage; ++binId)
        {
            if (mBinInstructions[binId] + newCluster.mInstructions <=
                mConfig.mInstructionsPerCluster)
            {
                mBinInstructions[binId] += newCluster.mInstructions;
                newCluster.mBinId = std::make_optional(binId);
                return true;
            }
        }
        // Revert the changes to the bins if we couldn't fit the new cluster.
        for (auto const& cluster : clustersToRemove)
        {
            mBinInstructions[cluster->mBinId.value()] += cluster->mInstructions;
        }
        return false;
    }

    // Remove conflicting clusters from mClusters in-place, saving them
    // in 'saved' for potential rollback.
    void
    removeConflictingClusters(
        std::unordered_set<Cluster const*> const& toRemove,
        std::vector<std::unique_ptr<Cluster const>>& saved)
    {
        size_t writePos = 0;
        for (size_t readPos = 0; readPos < mClusters.size(); ++readPos)
        {
            if (toRemove.find(mClusters[readPos].get()) != toRemove.end())
            {
                saved.push_back(std::move(mClusters[readPos]));
            }
            else
            {
                if (writePos != readPos)
                {
                    mClusters[writePos] = std::move(mClusters[readPos]);
                }
                ++writePos;
            }
        }
        mClusters.resize(writePos);
    }

    // Rollback an in-place mutation: find and remove the merged cluster,
    // then restore the saved conflicting clusters.
    void
    rollbackClusters(Cluster const* mergedCluster,
                     std::vector<std::unique_ptr<Cluster const>>& savedClusters)
    {
        // Find and swap-pop the merged cluster.
        for (size_t i = 0; i < mClusters.size(); ++i)
        {
            if (mClusters[i].get() == mergedCluster)
            {
                mClusters[i] = std::move(mClusters.back());
                mClusters.pop_back();
                break;
            }
        }
        // Restore the saved conflicting clusters.
        for (auto& saved : savedClusters)
        {
            mClusters.push_back(std::move(saved));
        }
    }

    // Simple bin-packing first-fit-decreasing heuristic
    // (https://en.wikipedia.org/wiki/First-fit-decreasing_bin_packing).
    // This has around 11/9 maximum approximation ratio, which probably has
    // the best complexity/performance tradeoff out of all the heuristics.
    bool
    binPacking(std::vector<std::unique_ptr<Cluster const>>& clusters,
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
                    newBinId[clusterId] = i;
                    packed = true;
                    break;
                }
            }
            if (!packed)
            {
                return false;
            }
        }
        for (size_t clusterId = 0; clusterId < clusters.size(); ++clusterId)
        {
            clusters[clusterId]->mBinId =
                std::make_optional(newBinId[clusterId]);
        }
        return true;
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
    std::vector<std::unique_ptr<Cluster const>> mClusters;
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
    std::vector<uint64_t> mBinInstructions;
    // Instructions in the least loaded bin; lets a conflict-free transaction
    // that can't fit into any bin be rejected in O(1).
    uint64_t mMinBinInstructions = 0;
    uint64_t mInstructions = 0;
    ParallelPartitionConfig mConfig;
    bool mTriedCompactingBinPacking = false;
    // Union of all clusters' mConflictTxs. Used as a fast-path check in
    // getConflictingClusters to avoid scanning all clusters when the
    // transaction has no conflicts with any existing cluster.
    BitSet mAllConflictTxs;
    // Maps tx id -> cluster pointer for O(K) conflict lookup.
    // Sized to the total number of transactions; nullptr means the tx
    // has not been added to this stage.
    std::vector<Cluster const*> mTxToCluster;
};

struct ParallelPhaseBuildResult
{
    TxStageFrameList mStages;
    std::vector<bool> mHadTxNotFittingLane;
    int64_t mTotalInclusionFee = 0;
};

ParallelPhaseBuildResult
buildSurgePricedParallelSorobanPhaseWithStageCount(
    std::vector<size_t> const& sortedTxOrder,
    std::vector<Resource> const& txResources, Resource const& laneLimit,
    std::vector<BuilderTx> const& builderTxs, TxFrameList const& txFrames,
    uint32_t stageCount, SorobanNetworkConfig const& sorobanCfg)
{
    ZoneScoped;
    ParallelPartitionConfig partitionCfg(stageCount, sorobanCfg);

    std::vector<Stage> stages;
    stages.reserve(partitionCfg.mStageCount);
    for (uint32_t i = 0; i < partitionCfg.mStageCount; ++i)
    {
        stages.emplace_back(partitionCfg, txFrames.size());
    }

    // Iterate transactions in decreasing fee order and try greedily pack them
    // into one of the stages until the limits are reached. Transactions that
    // don't fit into any of the stages are skipped and surge pricing will be
    // triggered for the transaction set.
    Resource laneLeft = laneLimit;
    bool hadTxNotFittingLane = false;

    for (size_t txIdx : sortedTxOrder)
    {
        auto const& txRes = txResources[txIdx];

        // Check if the transaction fits within the remaining lane resource
        // limits. This mirrors the anyGreater check in popTopTxs that skips
        // transactions exceeding resource limits.
        if (anyGreater(txRes, laneLeft))
        {
            hadTxNotFittingLane = true;
            continue;
        }

        // Try to add the transaction to one of the stages.
        bool added = false;
        for (auto& stage : stages)
        {
            if (stage.tryAdd(builderTxs[txIdx]))
            {
                added = true;
                break;
            }
        }

        if (added)
        {
            // Transaction included in the stage, update the remaining lane
            // resources.
            laneLeft -= txRes;
        }
        else
        {
            // Transaction didn't fit into any of the stages, mark that lane
            // limits were exceeded to trigger surge pricing.
            hadTxNotFittingLane = true;
        }
    }

    ParallelPhaseBuildResult result;
    result.mHadTxNotFittingLane = {hadTxNotFittingLane};

    // At this point the stages have been filled with transactions and we just
    // need to place the full transactions into the respective stages/clusters.
    result.mStages.reserve(stages.size());
    int64_t& totalInclusionFee = result.mTotalInclusionFee;
    for (auto const& stage : stages)
    {
        auto& resStage = result.mStages.emplace_back();
        resStage.reserve(partitionCfg.mClustersPerStage);

        std::vector<size_t> binToStageCluster(
            partitionCfg.mClustersPerStage, std::numeric_limits<size_t>::max());

        stage.visitAllTransactions([&resStage, &txFrames, &binToStageCluster,
                                    &totalInclusionFee](size_t binId,
                                                        size_t txId) {
            if (binToStageCluster[binId] == std::numeric_limits<size_t>::max())
            {
                binToStageCluster[binId] = resStage.size();
                resStage.emplace_back();
            }
            totalInclusionFee += txFrames[txId]->getInclusionFee();
            resStage[binToStageCluster[binId]].push_back(txFrames[txId]);
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
    while (!result.mStages.empty() && result.mStages.back().empty())
    {
        result.mStages.pop_back();
    }
    for (auto const& stage : result.mStages)
    {
        releaseAssert(!stage.empty());
    }

    return result;
}

// Runs `fn(threadId)` on `nThreads` threads (including the calling thread)
// and waits for all of them to finish.
void
runParallel(size_t nThreads, std::function<void(size_t)> const& fn)
{
    std::vector<std::thread> threads;
    threads.reserve(nThreads - 1);
    for (size_t t = 1; t < nThreads; ++t)
    {
        threads.emplace_back([&fn, t]() { fn(t); });
    }
    fn(0);
    for (auto& thread : threads)
    {
        thread.join();
    }
}

std::vector<BuilderTx>
prepareBuilderTxs(TxFrameList const& txFrames, size_t maxThreads)
{
    ZoneScoped;
    std::vector<BuilderTx> builderTxs;
    builderTxs.reserve(txFrames.size());
    for (size_t i = 0; i < txFrames.size(); ++i)
    {
        builderTxs.emplace_back(i, *txFrames[i]);
    }

    // Before trying to include any transactions, find all the pairs of the
    // conflicting transactions and mark the conflicts in the builderTxs.
    //
    // We use a grouping approach: collect all footprint entries into a flat
    // vector tagged with (key hash, tx id, RO/RW), group the entries by hash,
    // then scan for groups sharing the same key hash. This is significantly
    // faster in practice than using hash map lookups. The hashing, grouping
    // and conflict-marking passes are all parallelized across worker threads.
    //
    // This also has the further optimization potential: we could populate the
    // key maps and even the conflicting transactions eagerly in tx queue, thus
    // amortizing the costs across the whole ledger duration.
    struct FpEntry
    {
        uint64_t keyHash;
        uint32_t txId;
        bool isRW;
    };

    // Per-tx entry start offsets (exclusive prefix sums of the footprint
    // sizes), so that worker threads can fill disjoint ranges of the entry
    // array.
    std::vector<size_t> txEntryOffset(txFrames.size() + 1, 0);
    for (size_t i = 0; i < txFrames.size(); ++i)
    {
        auto const& fp = txFrames[i]->sorobanResources().footprint;
        txEntryOffset[i + 1] =
            txEntryOffset[i] + fp.readOnly.size() + fp.readWrite.size();
    }
    size_t const totalFpEntries = txEntryOffset.back();
    if (totalFpEntries == 0)
    {
        return builderTxs;
    }

    // Don't create threads with less than this many entries to process.
    size_t const MIN_ENTRIES_PER_THREAD = 1000;
    size_t const nThreads =
        std::max<size_t>(1, std::min<size_t>(maxThreads,
                                             totalFpEntries /
                                                 MIN_ENTRIES_PER_THREAD));

    // Hash all the footprint keys, chunking the transactions across threads
    // such that every chunk has a roughly equal number of keys.
    std::vector<FpEntry> fpEntries(totalFpEntries);
    std::vector<size_t> txChunkBound;
    txChunkBound.push_back(0);
    for (size_t t = 1; t < nThreads; ++t)
    {
        size_t entryTarget = totalFpEntries * t / nThreads;
        size_t txIdx = std::lower_bound(txEntryOffset.begin(),
                                        txEntryOffset.end(), entryTarget) -
                       txEntryOffset.begin();
        txChunkBound.push_back(
            std::clamp(txIdx, txChunkBound.back(), txFrames.size()));
    }
    txChunkBound.push_back(txFrames.size());
    runParallel(nThreads, [&](size_t t) {
        std::hash<LedgerKey> keyHasher;
        for (size_t i = txChunkBound[t]; i < txChunkBound[t + 1]; ++i)
        {
            auto const& footprint = txFrames[i]->sorobanResources().footprint;
            size_t pos = txEntryOffset[i];
            for (auto const& key : footprint.readOnly)
            {
                fpEntries[pos++] = {keyHasher(key), static_cast<uint32_t>(i),
                                    false};
            }
            for (auto const& key : footprint.readWrite)
            {
                fpEntries[pos++] = {keyHasher(key), static_cast<uint32_t>(i),
                                    true};
            }
        }
    });

    // Group the entries by hash: scatter them into buckets keyed by the top
    // bits of the (uniformly distributed) hash, then sort every bucket
    // independently. This replaces a single large sort with cheap linear
    // passes plus many small sorts that run in parallel.
    constexpr size_t BUCKET_BITS = 8;
    constexpr size_t BUCKET_COUNT = 1 << BUCKET_BITS;
    auto bucketOf = [](uint64_t hash) { return hash >> (64 - BUCKET_BITS); };
    auto entryChunkBound = [&](size_t t) {
        return totalFpEntries * t / nThreads;
    };

    std::vector<std::array<size_t, BUCKET_COUNT>> chunkBucketCount(
        nThreads, std::array<size_t, BUCKET_COUNT>{});
    runParallel(nThreads, [&](size_t t) {
        auto& counts = chunkBucketCount[t];
        for (size_t i = entryChunkBound(t); i < entryChunkBound(t + 1); ++i)
        {
            ++counts[bucketOf(fpEntries[i].keyHash)];
        }
    });

    // Compute the bucket boundaries and per-(chunk, bucket) write positions.
    std::vector<size_t> bucketStart(BUCKET_COUNT + 1, 0);
    std::vector<std::array<size_t, BUCKET_COUNT>> chunkWritePos(
        nThreads, std::array<size_t, BUCKET_COUNT>{});
    size_t pos = 0;
    for (size_t bucket = 0; bucket < BUCKET_COUNT; ++bucket)
    {
        bucketStart[bucket] = pos;
        for (size_t t = 0; t < nThreads; ++t)
        {
            chunkWritePos[t][bucket] = pos;
            pos += chunkBucketCount[t][bucket];
        }
    }
    bucketStart[BUCKET_COUNT] = pos;

    std::vector<FpEntry> bucketedEntries(totalFpEntries);
    runParallel(nThreads, [&](size_t t) {
        auto& writePos = chunkWritePos[t];
        for (size_t i = entryChunkBound(t); i < entryChunkBound(t + 1); ++i)
        {
            bucketedEntries[writePos[bucketOf(fpEntries[i].keyHash)]++] =
                fpEntries[i];
        }
    });

    // Scan every bucket for groups sharing the same hash, then mark
    // conflicts between transactions that share RW keys (RW-RW and RO-RW).
    // Conservatively treat hash collisions as potential conflicts - collisions
    // should generally be rare and allocating collisions to the same thread
    // is guaranteed to be safe (while disambiguating the conflicts would be
    // expensive and complex). Collision probability is really low (K^2/2^64).
    //
    // The conflicts are first collected as 'half-edges' (a directed
    // tx-conflicts-with-tx record) binned by the tx id that needs to be
    // updated, so that the conflict masks can then be updated in parallel
    // without any writer races: thread `t` owns the transactions with
    // `txId % nThreads == t`.
    struct HalfEdge
    {
        uint32_t mTxId;
        uint32_t mConflictTxId;
    };
    std::vector<std::vector<std::vector<HalfEdge>>> halfEdgeBins(
        nThreads, std::vector<std::vector<HalfEdge>>(nThreads));
    std::atomic<size_t> nextBucket{0};
    runParallel(nThreads, [&](size_t t) {
        auto& bins = halfEdgeBins[t];
        auto emitConflict = [&](uint32_t a, uint32_t b) {
            bins[a % nThreads].push_back({a, b});
            bins[b % nThreads].push_back({b, a});
        };
        std::vector<uint32_t> roTxs;
        std::vector<uint32_t> rwTxs;
        while (true)
        {
            size_t bucket = nextBucket.fetch_add(1);
            if (bucket >= BUCKET_COUNT)
            {
                break;
            }
            auto const begin = bucketedEntries.data() + bucketStart[bucket];
            auto const end = bucketedEntries.data() + bucketStart[bucket + 1];
            std::sort(begin, end, [](FpEntry const& a, FpEntry const& b) {
                return a.keyHash < b.keyHash;
            });
            for (auto groupStart = begin; groupStart != end;)
            {
                auto groupEnd = groupStart + 1;
                while (groupEnd != end &&
                       groupEnd->keyHash == groupStart->keyHash)
                {
                    ++groupEnd;
                }

                // Skip singleton groups — no possible conflicts.
                if (groupEnd - groupStart < 2)
                {
                    groupStart = groupEnd;
                    continue;
                }

                // Collect all entries with the matching key hash.
                roTxs.clear();
                rwTxs.clear();
                for (auto it = groupStart; it != groupEnd; ++it)
                {
                    (it->isRW ? rwTxs : roTxs).push_back(it->txId);
                }
                // RW-RW conflicts
                for (size_t i = 0; i < rwTxs.size(); ++i)
                {
                    for (size_t j = i + 1; j < rwTxs.size(); ++j)
                    {
                        // In a rare case of hash collision within a
                        // transaction, we might have the same transaction
                        // appear several times in the same group.
                        if (rwTxs[i] == rwTxs[j])
                        {
                            continue;
                        }
                        emitConflict(rwTxs[i], rwTxs[j]);
                    }
                }
                // RO-RW conflicts
                for (size_t i = 0; i < roTxs.size(); ++i)
                {
                    for (size_t j = 0; j < rwTxs.size(); ++j)
                    {
                        // In a rare case of hash collision within a
                        // transaction, we might have the same transaction
                        // appear several times in the same group.
                        if (roTxs[i] == rwTxs[j])
                        {
                            continue;
                        }
                        emitConflict(roTxs[i], rwTxs[j]);
                    }
                }

                groupStart = groupEnd;
            }
        }
    });

    // Apply the collected half-edges to the conflict masks.
    runParallel(nThreads, [&](size_t t) {
        for (size_t producer = 0; producer < nThreads; ++producer)
        {
            for (auto const& edge : halfEdgeBins[producer][t])
            {
                builderTxs[edge.mTxId].mConflictTxs.set(edge.mConflictTxId);
            }
        }
    });
    return builderTxs;
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
    // We prefer the transaction sets that are well utilized, but we also want
    // to lower the stage count when possible. Thus we will nominate a tx set
    // that has the lowest amount of stages while still being within
    // MAX_INCLUSION_FEE_TOLERANCE_FOR_STAGE_COUNT from the maximum total
    // inclusion fee (a proxy for the transaction set utilization).
    double const MAX_INCLUSION_FEE_TOLERANCE_FOR_STAGE_COUNT = 0.999;

    size_t const maxThreads =
        std::max(1, cfg.LEDGER_CLOSE_WORKER_THREADS);

    // Simplify the transactions to the minimum necessary amount of data on
    // worker threads, overlapped with the fee-order sort and the resource
    // precomputation below (these are independent of the conflict
    // discovery).
    std::vector<BuilderTx> builderTxs;
    std::thread prepareThread(
        [&]() { builderTxs = prepareBuilderTxs(txFrames, maxThreads); });

    // Sort transactions in decreasing inclusion fee order.
    TxFeeComparator txComparator(
        /* isGreater */ true,
        stellar::rand_uniform<size_t>(0, std::numeric_limits<size_t>::max()));
    std::vector<size_t> sortedTxOrder(txFrames.size());
    std::iota(sortedTxOrder.begin(), sortedTxOrder.end(), 0);
    std::sort(sortedTxOrder.begin(), sortedTxOrder.end(),
              [&txFrames, &txComparator](size_t a, size_t b) {
                  return txComparator(txFrames[a], txFrames[b]);
              });

    // Precompute per-transaction resources to avoid repeated virtual calls
    // and heap allocations across threads.
    std::vector<Resource> txResources;
    txResources.reserve(txFrames.size());
    for (auto const& tx : txFrames)
    {
        txResources.push_back(
            tx->getResources(/* useByteLimitInClassic */ false, ledgerVersion));
    }
    prepareThread.join();

    // Get the lane limit. Soroban uses a single generic lane.
    auto const& laneLimits = laneConfig->getLaneLimits();
    releaseAssert(laneLimits.size() == 1);
    auto const& laneLimit = laneLimits[0];

    // Create a worker thread for each stage count. The sorted order and
    // precomputed resources are shared across all threads (read-only).
    std::vector<std::thread> threads;
    uint32_t stageCountOptions = cfg.SOROBAN_PHASE_MAX_STAGE_COUNT -
                                 cfg.SOROBAN_PHASE_MIN_STAGE_COUNT + 1;
    std::vector<ParallelPhaseBuildResult> results(stageCountOptions);

    for (uint32_t stageCount = cfg.SOROBAN_PHASE_MIN_STAGE_COUNT;
         stageCount <= cfg.SOROBAN_PHASE_MAX_STAGE_COUNT; ++stageCount)
    {
        size_t resultIndex = stageCount - cfg.SOROBAN_PHASE_MIN_STAGE_COUNT;
        threads.emplace_back([&sortedTxOrder, &txResources, &laneLimit,
                              &builderTxs, &txFrames, stageCount, &sorobanCfg,
                              resultIndex, &results]() {
            results.at(resultIndex) =
                buildSurgePricedParallelSorobanPhaseWithStageCount(
                    sortedTxOrder, txResources, laneLimit, builderTxs, txFrames,
                    stageCount, sorobanCfg);
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
    std::optional<size_t> bestResultIndex = std::nullopt;
    for (size_t i = 0; i < results.size(); ++i)
    {
        CLOG_DEBUG(Herder,
                   "Parallel Soroban tx set nomination: {} stages => {} total "
                   "inclusion fee",
                   results[i].mStages.size(), results[i].mTotalInclusionFee);
        if (results[i].mTotalInclusionFee < maxTotalInclusionFee)
        {
            continue;
        }
        if (!bestResultIndex ||
            results[i].mStages.size() <
                results[bestResultIndex.value()].mStages.size())
        {
            bestResultIndex = std::make_optional(i);
        }
    }
    releaseAssert(bestResultIndex.has_value());
    auto& bestResult = results[bestResultIndex.value()];
    hadTxNotFittingLane = std::move(bestResult.mHadTxNotFittingLane);
    return std::move(bestResult.mStages);
}

} // namespace stellar
