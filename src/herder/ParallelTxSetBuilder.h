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
// Candidates are validated lazily through `isValid`, on the calling thread:
// only the transactions that a packing pass actually placed (invalid ones are
// dropped and the pass repeated), plus the non-fitting transactions, probed
// in fee order until a valid one establishes excess demand. Transactions
// never reached are neither validated nor included.
TxStageFrameList buildSurgePricedParallelSorobanPhase(
    TxFrameList const& txFrames, Config const& cfg,
    SorobanNetworkConfig const& sorobanCfg,
    std::shared_ptr<SurgePricingLaneConfig> laneConfig,
    std::vector<bool>& hadTxNotFittingLane, uint32_t ledgerVersion,
    TxValidationCallback const& isValid);

} // namespace stellar
