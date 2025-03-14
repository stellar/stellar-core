#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

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
    std::vector<bool>& hadTxNotFittingLane);

} // namespace stellar
