// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/InvokeHostFunctionOpFrame.h"

#include "ledger/LedgerManager.h"
#include "ledger/NetworkConfig.h"
#include "main/AppConnector.h"
#include "transactions/MutableTransactionResult.h"
#include "util/GlobalChecks.h"
#include "util/ProtocolVersion.h"
#include <stdexcept>

namespace stellar
{

InvokeHostFunctionOpFrame::InvokeHostFunctionOpFrame(
    Operation const& op, TransactionFrame const& parentTx)
    : OperationFrame(op, parentTx)
    , mInvokeHostFunction(mOperation.body.invokeHostFunctionOp())
{
}

bool
InvokeHostFunctionOpFrame::isOpSupported(LedgerHeader const& header) const
{
    return protocolVersionStartsFrom(header.ledgerVersion,
                                     SOROBAN_PROTOCOL_VERSION);
}

bool
InvokeHostFunctionOpFrame::doApplyForSoroban(
    AppConnector& app, AbstractLedgerTxn& ltx,
    SorobanNetworkConfig const& sorobanConfig, Hash const& sorobanBasePrngSeed,
    OperationResult& res,
    std::optional<RefundableFeeTracker>& refundableFeeTracker,
    OperationMetaBuilder& opMeta) const
{
    // Soroban apply has fully moved to Rust (see
    // LedgerManagerImpl::applySorobanPhaseRust). The C++ op-frame apply
    // path is no longer reachable.
    releaseAssert(false);
}

bool
InvokeHostFunctionOpFrame::doApply(AppConnector& app, AbstractLedgerTxn& ltx,
                                   OperationResult& res,
                                   OperationMetaBuilder& opMeta) const
{
    throw std::runtime_error(
        "InvokeHostFunctionOpFrame may only be applied via doApplyForSoroban");
}

bool
InvokeHostFunctionOpFrame::doCheckValidForSoroban(
    SorobanNetworkConfig const& networkConfig, Config const& appConfig,
    uint32_t ledgerVersion, OperationResult& res,
    DiagnosticEventManager& diagnosticEvents) const
{
    // check wasm size if uploading contract
    auto const& hostFn = mInvokeHostFunction.hostFunction;
    if (hostFn.type() == HOST_FUNCTION_TYPE_UPLOAD_CONTRACT_WASM &&
        hostFn.wasm().size() > networkConfig.maxContractSizeBytes())
    {
        diagnosticEvents.pushError(
            SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
            "uploaded Wasm size exceeds network config maximum contract size",
            {makeU64SCVal(hostFn.wasm().size()),
             makeU64SCVal(networkConfig.maxContractSizeBytes())});
        return false;
    }
    if (hostFn.type() == HOST_FUNCTION_TYPE_CREATE_CONTRACT)
    {
        auto const& preimage = hostFn.createContract().contractIDPreimage;
        if (preimage.type() == CONTRACT_ID_PREIMAGE_FROM_ASSET &&
            !isAssetValid(preimage.fromAsset(), ledgerVersion))
        {
            diagnosticEvents.pushError(SCE_VALUE, SCEC_INVALID_INPUT,
                                       "invalid asset to create contract from");
            return false;
        }
    }
    return true;
}

bool
InvokeHostFunctionOpFrame::doCheckValid(uint32_t ledgerVersion,
                                        OperationResult& res) const
{
    throw std::runtime_error(
        "InvokeHostFunctionOpFrame::doCheckValid needs Config");
}

void
InvokeHostFunctionOpFrame::insertLedgerKeysToPrefetch(
    UnorderedSet<LedgerKey>& keys) const
{
}

bool
InvokeHostFunctionOpFrame::isSoroban() const
{
    return true;
}

bool
InvokeHostFunctionOpFrame::doesAccessFrozenKey(
    SorobanNetworkConfig const& sorobanConfig) const
{
    // Soroban footprint checks happen at transaction level, so we can safely
    // say that the operation itself doesn't access frozen keys.
    return false;
}
}
