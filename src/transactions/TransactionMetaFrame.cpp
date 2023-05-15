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

namespace stellar
{
TransactionMetaFrame::TransactionMetaFrame(uint32_t protocolVersion)
{
    // The TransactionMeta v() switch can be in 4 positions 0, 1, 2, 3. We
    // do not support 0 or 1 at all -- core does not produce it anymore and we
    // have no obligation to consume it under any circumstance -- so this
    // class just switches between cases 2 and 3.
    mVersion = 2;
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    if (protocolVersionStartsFrom(protocolVersion, SOROBAN_PROTOCOL_VERSION))
    {
        mVersion = 3;
    }
#endif
    mTransactionMeta.v(mVersion);
}

template <typename T>
void
vecAppend(xdr::xvector<T>& a, xdr::xvector<T>&& b)
{
    std::move(b.begin(), b.end(), std::back_inserter(a));
}

size_t
TransactionMetaFrame::getNumChangesBefore() const
{
    switch (mTransactionMeta.v())
    {
    case 2:
        return mTransactionMeta.v2().txChangesBefore.size();
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    case 3:
        return mTransactionMeta.v3().txChangesBefore.size();
#endif
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
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    case 3:
        return mTransactionMeta.v3().txChangesBefore;
#endif
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
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    case 3:
        vecAppend(mTransactionMeta.v3().txChangesBefore, std::move(changes));
        break;
#endif
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
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    case 3:
        mTransactionMeta.v3().operations.clear();
        break;
#endif
    default:
        releaseAssert(false);
    }
}

void
TransactionMetaFrame::pushOperationMetas(xdr::xvector<OperationMeta>&& opMetas)
{
    switch (mTransactionMeta.v())
    {
    case 2:
        vecAppend(mTransactionMeta.v2().operations, std::move(opMetas));
        break;
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    case 3:
        vecAppend(mTransactionMeta.v3().operations, std::move(opMetas));
        break;
#endif
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
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    case 3:
        return mTransactionMeta.v3().operations.size();
#endif
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
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    case 3:
        vecAppend(mTransactionMeta.v3().txChangesAfter, std::move(changes));
        break;
#endif
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
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    case 3:
        mTransactionMeta.v3().txChangesAfter.clear();
        break;
#endif
    default:
        releaseAssert(false);
    }
}

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
void
TransactionMetaFrame::pushContractEvents(xdr::xvector<ContractEvent>&& events)
{
    switch (mTransactionMeta.v())
    {
    case 2:
        // Do nothing, until v3 we don't create events.
        break;
    case 3:
        mTransactionMeta.v3().events = std::move(events);
        break;
    default:
        releaseAssert(false);
    }
}

void
TransactionMetaFrame::pushDiagnosticEvents(
    xdr::xvector<DiagnosticEvent>&& events)
{
    switch (mTransactionMeta.v())
    {
    case 2:
        // Do nothing, until v3 we don't create events.
        break;
    case 3:
        mTransactionMeta.v3().diagnosticEvents = std::move(events);
        break;
    default:
        releaseAssert(false);
    }
}

void
TransactionMetaFrame::pushReturnValues(
    xdr::xvector<SCVal, MAX_OPS_PER_TX>&& returnValues)
{
    switch (mTransactionMeta.v())
    {
    case 2:
        // Do nothing, until v3 we don't call into contracts.
        break;
    case 3:
        mTransactionMeta.v3().returnValues = std::move(returnValues);
        break;
    default:
        releaseAssert(false);
    }
}

#endif

TransactionMeta const&
TransactionMetaFrame::getXDR() const
{
    return mTransactionMeta;
}
}
