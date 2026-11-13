// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "herder/SurgePricingUtils.h"
#include "herder/TxSetFrame.h"
#include "ledger/NetworkConfig.h"
#include "main/Config.h"

namespace stellar
{
// Builds a sequence of parallel processing stages from the provided
// transactions while respecting the limits defined by the network
// configuration.
// The number of stages and the number of clusters in each stage is determined
// by the provided configurations (`cfg` and `sorobanCfg`).
// The resource limits in transactions are determined based on the input
// `laneConfig`.
// This doesn't support multi-lane surge pricing and thus it's expected
// `laneConfig` to only have a configuration for a single surge pricing lane.
TxStageFrameList buildSurgePricedParallelSorobanPhase(
    TxFrameList const& txFrames, Config const& cfg,
    SorobanNetworkConfig const& sorobanCfg,
    std::shared_ptr<SurgePricingLaneConfig> laneConfig,
    std::vector<bool>& hadTxNotFittingLane, uint32_t ledgerVersion,
    // Randomizes the order of transactions with equal fee rates. Drawn from
    // the global random engine (main thread only) unless provided.
    std::optional<size_t> seed = std::nullopt);

// Adds transactions to a parallel Soroban phase that has been selected
// already, without selecting the phase again.
//
// A leader selects the set for the next ledger before the current one has
// closed, and completes it at the close: what turned out invalid is removed
// (a phase stays valid when transactions are taken out of it), and what
// arrived since is added. Selecting the whole phase again
// (`buildSurgePricedParallelSorobanPhase`) costs as much as the first time;
// this adds one transaction at a time to the stages that exist.
//
// A transaction goes into the first stage where it conflicts with at most
// one cluster: into that cluster, or, without a conflict, into the cluster
// with the fewest instructions (a new one while the stage has fewer than
// `ledgerMaxDependentTxClusters`). It is only added if the result is within
// the limits a set is validated against: the lane's resources, and the
// ledger's instruction limit for the sum over the stages of each stage's
// largest cluster. Clusters are never merged and stages are never added (a
// phase without stages gets one): a transaction that would need either is not
// added. The result is checked like any selection
// (`TxSetBuilder::checkSelection`) by whoever uses it.
class ParallelPhaseExtender
{
  public:
    // `stages` is the phase so far; it may be empty. `laneLimit` is the
    // limit of the phase's (only) surge pricing lane.
    ParallelPhaseExtender(TxStageFrameList stages,
                          SorobanNetworkConfig const& sorobanCfg,
                          Resource const& laneLimit, uint32_t ledgerVersion);

    // Adds `tx` if it fits; returns whether it did.
    bool tryAdd(TransactionFrameBasePtr const& tx);

    size_t
    sizeTx() const
    {
        return mTxCount;
    }

    TxStageFrameList const&
    getStages() const
    {
        return mStages;
    }

  private:
    struct StageState
    {
        std::vector<uint64_t> mClusterInstructions;
        uint64_t mMaxClusterInstructions{0};
        // Hash of a footprint key -> the cluster that writes it. One cluster
        // at most: two clusters of a stage never share a written key.
        std::unordered_map<size_t, uint32_t> mWriters;
        // Hash of a footprint key -> the clusters that only read it.
        std::unordered_map<size_t, std::vector<uint32_t>> mReaders;
    };

    void index(StageState& stage, uint32_t cluster,
               TransactionFrameBase const& tx);
    // The clusters of `stage` that `tx` conflicts with: none, one, or (as
    // nullopt) more than one.
    std::optional<std::optional<uint32_t>>
    conflictingCluster(StageState const& stage,
                       TransactionFrameBase const& tx) const;
    bool withinInstructionLimit(StageState const& stage,
                                uint64_t clusterInstructions) const;

    TxStageFrameList mStages;
    std::vector<StageState> mState;
    Resource mLaneLimit;
    Resource mTotal;
    uint32_t const mLedgerVersion;
    uint32_t const mMaxClusters;
    uint64_t const mMaxInstructions;
    // Sum over the stages of their largest cluster's instructions.
    uint64_t mSequentialInstructions{0};
    size_t mTxCount{0};
};

} // namespace stellar
