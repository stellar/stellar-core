// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/TransactionMetaFrame.h"
#include "crypto/SHA.h"
#include "transactions/TransactionFrameBase.h"
#include "util/GlobalChecks.h"
#include "util/MetaUtils.h"
#include "util/ProtocolVersion.h"
#include <iterator>
#include <xdrpp/xdrpp/marshal.h>

namespace
{
void
setSorobanMetaFeeInfo(stellar::SorobanTransactionMetaExt& sorobanMetaExt,
                      int64_t nonRefundableFeeSpent,
                      int64_t totalRefundableFeeSpent, int64_t rentFeeCharged)
{
    auto& ext = sorobanMetaExt.v1();
    ext.totalNonRefundableResourceFeeCharged = nonRefundableFeeSpent;
    ext.totalRefundableResourceFeeCharged = totalRefundableFeeSpent;
    ext.rentFeeCharged = rentFeeCharged;
}

template <typename T>
void
vecAppend(xdr::xvector<T>& a, xdr::xvector<T>&& b)
{
    std::move(b.begin(), b.end(), std::back_inserter(a));
}

}

namespace stellar
{

TransactionMetaFrame::TransactionMetaFrame(uint32_t protocolVersion,
                                           Config const& config)
{
    // The TransactionMeta v() switch can be in 5 positions 0, 1, 2, 3, 4. We do
    // not support 0 or 1 at all -- core does not produce it anymore and we have
    // no obligation to consume it under any circumstance -- so this class just
    // switches between cases 2, 3 and 4.
    if (protocolVersionStartsFrom(protocolVersion, ProtocolVersion::V_23) ||
        config.BACKFILL_STELLAR_ASSET_EVENTS)
    {
        mVersion = 4;
    }
    else if (protocolVersionStartsFrom(protocolVersion,
                                       SOROBAN_PROTOCOL_VERSION))
    {
        mVersion = 3;
    }
    else
    {
        mVersion = 2;
    }
    mTransactionMeta.v(mVersion);
}

size_t
TransactionMetaFrame::getNumChangesBefore() const
{
    switch (mTransactionMeta.v())
    {
    case 2:
        return mTransactionMeta.v2().txChangesBefore.size();
    case 3:
        return mTransactionMeta.v3().txChangesBefore.size();
    case 4:
        return mTransactionMeta.v4().txChangesBefore.size();
    default:
        releaseAssert(false);
    }
}

LedgerEntryChanges
TransactionMetaFrame::getChangesBefore() const
{
    switch (mTransactionMeta.v())
    {
    case 2:
        return mTransactionMeta.v2().txChangesBefore;
    case 3:
        return mTransactionMeta.v3().txChangesBefore;
    case 4:
        return mTransactionMeta.v4().txChangesBefore;
    default:
        releaseAssert(false);
    }
}

LedgerEntryChanges
TransactionMetaFrame::getChangesAfter() const
{
    switch (mTransactionMeta.v())
    {
    case 2:
        return mTransactionMeta.v2().txChangesAfter;
    case 3:
        return mTransactionMeta.v3().txChangesAfter;
    case 4:
        return mTransactionMeta.v4().txChangesAfter;
    default:
        releaseAssert(false);
    }
}

void
TransactionMetaFrame::pushTxChangesBefore(LedgerEntryChanges&& changes)
{
    switch (mTransactionMeta.v())
    {
    case 2:
        vecAppend(mTransactionMeta.v2().txChangesBefore, std::move(changes));
        break;
    case 3:
        vecAppend(mTransactionMeta.v3().txChangesBefore, std::move(changes));
        break;
    case 4:
        vecAppend(mTransactionMeta.v4().txChangesBefore, std::move(changes));
        break;
    default:
        releaseAssert(false);
    }
}

void
TransactionMetaFrame::clearOperationMetas()
{
    switch (mTransactionMeta.v())
    {
    case 2:
        mTransactionMeta.v2().operations.clear();
        break;
    case 3:
        mTransactionMeta.v3().operations.clear();
        break;
    case 4:
        mTransactionMeta.v4().operations.clear();
        break;
    default:
        releaseAssert(false);
    }
}

void
TransactionMetaFrame::pushOperationMetas(OperationMetaArray&& opMetas)
{
    switch (mTransactionMeta.v())
    {
    case 2:
        vecAppend(mTransactionMeta.v2().operations, opMetas.convertToXDR());
        break;
    case 3:
        vecAppend(mTransactionMeta.v3().operations, opMetas.convertToXDR());
        break;
    case 4:
        vecAppend(mTransactionMeta.v4().operations, opMetas.convertToXDRV2());
        break;
    default:
        releaseAssert(false);
    }
}

size_t
TransactionMetaFrame::getNumOperations() const
{
    switch (mTransactionMeta.v())
    {
    case 2:
        return mTransactionMeta.v2().operations.size();
    case 3:
        return mTransactionMeta.v3().operations.size();
    case 4:
        return mTransactionMeta.v4().operations.size();
    default:
        releaseAssert(false);
    }
}

void
TransactionMetaFrame::pushTxChangesAfter(LedgerEntryChanges&& changes)
{
    switch (mTransactionMeta.v())
    {
    case 2:
        vecAppend(mTransactionMeta.v2().txChangesAfter, std::move(changes));
        break;
    case 3:
        vecAppend(mTransactionMeta.v3().txChangesAfter, std::move(changes));
        break;
    case 4:
        vecAppend(mTransactionMeta.v4().txChangesAfter, std::move(changes));
        break;
    default:
        releaseAssert(false);
    }
}

void
TransactionMetaFrame::clearTxChangesAfter()
{
    switch (mTransactionMeta.v())
    {
    case 2:
        mTransactionMeta.v2().txChangesAfter.clear();
        break;
    case 3:
        mTransactionMeta.v3().txChangesAfter.clear();
        break;
    case 4:
        mTransactionMeta.v4().txChangesAfter.clear();
        break;
    default:
        releaseAssert(false);
    }
}

void
TransactionMetaFrame::maybePushSorobanContractEvents(
    OperationMetaArray& opMetas)
{
    switch (mTransactionMeta.v())
    {
    case 2:
        // Do nothing, until v3 we don't create events.
        break;
    case 3:
        mTransactionMeta.v3().sorobanMeta.activate().events =
            opMetas.flushContractEvents();
        break;
    case 4:
        // Do nothing, v4 soroban contract events live in the operation meta
        break;
    default:
        releaseAssert(false);
    }
}

void
TransactionMetaFrame::pushTxContractEvents(xdr::xvector<ContractEvent>&& events)
{
    switch (mTransactionMeta.v())
    {
    case 2:
    case 3:
        // Do nothing, until v4 we don't have Tx-level contract events.
        // v3 soroban contract events should be populated via
        // `maybePushSorobanContractEvents`.
        break;
    case 4:
        mTransactionMeta.v4().events = std::move(events);
        break;
    default:
        releaseAssert(false);
    }
}

void
TransactionMetaFrame::maybePushDiagnosticEvents(
    xdr::xvector<DiagnosticEvent>&& events, bool isSoroban)
{
    switch (mTransactionMeta.v())
    {
    case 2:
        // Do nothing, until v3 we don't create events.
        break;
    case 3:
        // In V3 only activate sorobanMeta if it's a Soroban transaction
        if (isSoroban)
        {
            mTransactionMeta.v3().sorobanMeta.activate().diagnosticEvents =
                std::move(events);
        }
        break;
    case 4:
        if (isSoroban)
        {
            mTransactionMeta.v4().diagnosticEvents = std::move(events);
        }
        break;
    default:
        releaseAssert(false);
    }
}

void
TransactionMetaFrame::setReturnValue(SCVal&& returnValue)
{
    switch (mTransactionMeta.v())
    {
    case 2:
        // Do nothing, until v3 we don't call into contracts.
        break;
    case 3:
        mTransactionMeta.v3().sorobanMeta.activate().returnValue =
            std::move(returnValue);
        break;
    case 4:
        mTransactionMeta.v4().sorobanMeta.activate().returnValue =
            std::move(returnValue);
        break;
    default:
        releaseAssert(false);
    }
}

#ifdef BUILD_TESTS
TransactionMetaFrame::TransactionMetaFrame(TransactionMeta meta)
    : mTransactionMeta(meta), mVersion(meta.v())
{
}

SCVal const&
TransactionMetaFrame::getReturnValue() const
{
    switch (mTransactionMeta.v())
    {
    case 2:
        throw std::runtime_error("Return value not available for v2 meta");
    case 3:
        return mTransactionMeta.v3().sorobanMeta->returnValue;
    case 4:
        return mTransactionMeta.v4().sorobanMeta->returnValue;
    default:
        releaseAssert(false);
    }
}

xdr::xvector<stellar::DiagnosticEvent> const&
TransactionMetaFrame::getDiagnosticEvents() const
{
    switch (mTransactionMeta.v())
    {
    case 2:
        throw std::runtime_error("Diagnostic events not available for v2 meta");
    case 3:
        return mTransactionMeta.v3().sorobanMeta->diagnosticEvents;
    case 4:
        return mTransactionMeta.v4().diagnosticEvents;
    default:
        releaseAssert(false);
    }
}

xdr::xvector<stellar::ContractEvent> const&
TransactionMetaFrame::getTxEvents() const
{
    switch (mTransactionMeta.v())
    {
    case 2:
    case 3:
        throw std::runtime_error(
            "Transaction-level contract events not available for v2/v3 meta");
    case 4:
        return mTransactionMeta.v4().events;
    default:
        releaseAssert(false);
    }
}

xdr::xvector<stellar::ContractEvent>
TransactionMetaFrame::getSorobanContractEvents() const
{
    switch (mTransactionMeta.v())
    {
    case 2:
        throw std::runtime_error("Contract events not available for v2 meta");
    case 3:
        return mTransactionMeta.v3().sorobanMeta->events;
    case 4:
        if (mTransactionMeta.v4().operations.empty())
        {
            return xdr::xvector<stellar::ContractEvent>{};
        }
        else if (mTransactionMeta.v4().operations.size() == 1)
        {
            return mTransactionMeta.v4().operations.at(0).events;
        }
        else
        {
            throw std::runtime_error("Operation meta size can only be 0 or 1 "
                                     "in a Soroban transaction");
        }
    default:
        releaseAssert(false);
    }
}

stellar::LedgerEntryChanges const&
TransactionMetaFrame::getLedgerEntryChangesAtOp(size_t opIdx) const
{
    switch (mTransactionMeta.v())
    {
    case 2:
        return mTransactionMeta.v2().operations.at(opIdx).changes;
    case 3:
        return mTransactionMeta.v3().operations.at(opIdx).changes;
    case 4:
        return mTransactionMeta.v4().operations.at(opIdx).changes;
    default:
        releaseAssert(false);
    }
}

xdr::xvector<ContractEvent> const&
TransactionMetaFrame::getOpEventsAtOp(size_t opIdx) const
{
    switch (mTransactionMeta.v())
    {
    case 2:
    case 3:
        throw std::runtime_error(
            "Operation events not available for v2/v3 meta");
    case 4:
        return mTransactionMeta.v4().operations.at(opIdx).events;
    default:
        releaseAssert(false);
    }
}

#endif

void
TransactionMetaFrame::setSorobanFeeInfo(int64_t nonRefundableFeeSpent,
                                        int64_t totalRefundableFeeSpent,
                                        int64_t rentFeeCharged)
{
    switch (mTransactionMeta.v())
    {
    case 2:
        // Do nothing, until v3 we don't call into contracts.
        break;
    case 3:
        setSorobanMetaFeeInfo(mTransactionMeta.v3().sorobanMeta.activate().ext,
                              nonRefundableFeeSpent, totalRefundableFeeSpent,
                              rentFeeCharged);
        break;
    case 4:
        setSorobanMetaFeeInfo(mTransactionMeta.v4().sorobanMeta.activate().ext,
                              nonRefundableFeeSpent, totalRefundableFeeSpent,
                              rentFeeCharged);
        break;
    default:
        releaseAssert(false);
    }
}

TransactionMeta const&
TransactionMetaFrame::getXDR() const
{
    return mTransactionMeta;
}
}
