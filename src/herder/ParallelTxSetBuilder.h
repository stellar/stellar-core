// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "herder/SurgePricingUtils.h"
#include "herder/TxSetFrame.h"

namespace stellar
{
class Application;

// Builds a sequence of parallel processing stages from the provided
// transactions while respecting the limits defined by the network
// configuration.
// The number of stages and the number of clusters in each stage is determined
// by the node configuration and the last closed Soroban network configuration.
// The resource limits in transactions are determined based on the input
// `laneConfig`.
// This doesn't support multi-lane surge pricing and thus it's expected
// `laneConfig` to only have a configuration for a single surge pricing lane.
TxStageFrameList buildSurgePricedParallelSorobanPhase(
    Application& app, TxFrameList const& txFrames,
    std::shared_ptr<SurgePricingLaneConfig> laneConfig,
    std::vector<bool>& hadTxNotFittingLane, uint32_t ledgerVersion);

} // namespace stellar
