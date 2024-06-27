// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "herder/TxSetUtils.h"
#include "xdr/Stellar-ledger.h"

namespace stellar
{
namespace testtxset
{

using PhaseComponents = std::vector<
    std::pair<std::optional<int64_t>, std::vector<TransactionFrameBasePtr>>>;
std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
makeNonValidatedGeneralizedTxSet(
    std::vector<PhaseComponents> const& txsPerBaseFee, Application& app,
    Hash const& previousLedgerHash,
    std::optional<bool> useParallelSorobanPhase = std::nullopt);

std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
makeNonValidatedTxSetBasedOnLedgerVersion(
    std::vector<TransactionFrameBasePtr> const& txs, Application& app,
    Hash const& previousLedgerHash);
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
void normalizeParallelPhaseXDR(TransactionPhase& phase);

std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
makeNonValidatedGeneralizedTxSet(PhaseComponents const& classicTxsPerBaseFee,
                                 std::optional<int64_t> sorobanBaseFee,
                                 TxStageFrameList const& sorobanTxsPerStage,
                                 Application& app,
                                 Hash const& previousLedgerHash);
#endif
} // namespace testtxset
} // namespace stellar
