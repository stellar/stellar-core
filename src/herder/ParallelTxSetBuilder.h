#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/SurgePricingUtils.h"
#include "herder/TxSetFrame.h"
#include "ledger/NetworkConfig.h"
#include "main/Config.h"
#include "util/BitSet.h"

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
    std::vector<bool>& hadTxNotFittingLane, uint32_t ledgerVersion);

#ifdef BUILD_TESTS
// Test function that builds a simple TxStageFrameList with fixed parallelism
// and target number of stages, ignoring resource limits and surge pricing.
// - txFrames: input transactions to partition
// - clustersPerStage: fixed number of clusters per stage
// - targetStageCount: target number of stages (actual may be more if txs spill
// over) Assigns transactions to non-conflicting clusters sequentially,
// buffering any that don't fit due to conflicts for the next stage.
TxStageFrameList buildSimpleParallelTxStages(TxFrameList const& txFrames,
                                             uint32_t clustersPerStage,
                                             uint32_t targetStageCount);

// Test-only export of the core algorithm that works with indices
// Returns stages[stage][cluster][tx_index]
std::vector<std::vector<std::vector<size_t>>>
testBuildSimpleParallelStagesFromIndices(
    std::vector<BitSet> const& conflictSets, uint32_t clustersPerStage,
    uint32_t targetStageCount);
#endif

} // namespace stellar
