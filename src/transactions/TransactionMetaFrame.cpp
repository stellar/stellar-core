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
    releaseAssert(!mHashesFinalized);
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
    releaseAssert(!mHashesFinalized);
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
    releaseAssert(!mHashesFinalized);
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
    releaseAssert(!mHashesFinalized);
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
    releaseAssert(!mHashesFinalized);
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

void
TransactionMetaFrame::setTxResult(TransactionResult const& res)
{
    releaseAssert(!mHashesFinalized);
    switch (mTransactionMeta.v())
    {
    case 2:
        // Do nothing, until v3 we emit the txresult elsewhere.
        break;
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    case 3:
        mTransactionMeta.v3().txResult = res;
        break;
#endif
    default:
        releaseAssert(false);
    }
}

void
TransactionMetaFrame::finalizeHashes()
{
    releaseAssert(!mHashesFinalized);
    switch (mTransactionMeta.v())
    {
    case 2:
        // Do nothing, until v3 we have no hashes.
        break;
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    case 3:
    {
        // Calculate 3 hashes as described in CAP-0056:
        //
        //  hash[0] = sha256(txChangesBefore, operations, txChangesAfter)
        //  hash[1] = sha256(events)
        //  hash[2] = sha256(txResult)
        //
        // These will, in turn, be combined into a single hash in
        // getHashOfMetaHashes below, which fills in the
        // TransactionResultPairV2.hashOfMetaHashes value in various contexts.

        normalizeMeta(mTransactionMeta);

        SHA256 sha;

        sha.add(xdr::xdr_to_opaque(mTransactionMeta.v3().txChangesBefore));
        sha.add(xdr::xdr_to_opaque(mTransactionMeta.v3().operations));
        sha.add(xdr::xdr_to_opaque(mTransactionMeta.v3().txChangesAfter));
        mTransactionMeta.v3().hashes[0] = sha.finish();

        sha.reset();
        sha.add(xdr::xdr_to_opaque(mTransactionMeta.v3().events));
        mTransactionMeta.v3().hashes[1] = sha.finish();

        sha.reset();
        sha.add(xdr::xdr_to_opaque(mTransactionMeta.v3().txResult));
        mTransactionMeta.v3().hashes[2] = sha.finish();
    }
    break;
#endif
    default:
        releaseAssert(false);
    }
    mHashesFinalized = true;
}

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
void
TransactionMetaFrame::pushContractEvents(xdr::xvector<ContractEvent>&& events)
{
    releaseAssert(!mHashesFinalized);
    switch (mTransactionMeta.v())
    {
    case 2:
        // Do nothing, until v3 we don't create events.
        break;
    case 3:
        mTransactionMeta.v3().events.emplace_back(
            OperationEvents{std::move(events)});
        break;
    default:
        releaseAssert(false);
    }
}

Hash
TransactionMetaFrame::getHashOfMetaHashes(TransactionMeta const& tm)
{
    // We should only ever be calling this on a v3 txmeta.
    releaseAssert(tm.v() == 3);
    SHA256 sha;
    for (auto h : tm.v3().hashes)
    {
        sha.add(h);
    }
    return sha.finish();
}
#endif

TransactionMeta const&
TransactionMetaFrame::getXDR() const
{
    releaseAssert(mHashesFinalized);
    return mTransactionMeta;
}
}
