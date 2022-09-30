// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerCloseMetaFrame.h"
#include "crypto/SHA.h"
#include "transactions/TransactionMetaFrame.h"
#include "util/GlobalChecks.h"
#include "util/ProtocolVersion.h"

namespace stellar
{
LedgerCloseMetaFrame::LedgerCloseMetaFrame(uint32_t protocolVersion)
{
    // The LedgerCloseMeta v() switch can be in 3 positions 0, 1, 2. We
    // currently support all 3 of these cases, depending on both compile time
    // and runtime conditions.
    mVersion = 0;
    if (protocolVersionStartsFrom(protocolVersion,
                                  GENERALIZED_TX_SET_PROTOCOL_VERSION))
    {
        mVersion = 1;
    }
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    if (protocolVersionStartsFrom(protocolVersion, SOROBAN_PROTOCOL_VERSION))
    {
        mVersion = 2;
    }
#endif
    mLedgerCloseMeta.v(mVersion);
}

LedgerHeaderHistoryEntry&
LedgerCloseMetaFrame::ledgerHeader()
{
    switch (mVersion)
    {
    case 0:
        return mLedgerCloseMeta.v0().ledgerHeader;
    case 1:
        return mLedgerCloseMeta.v1().ledgerHeader;
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    case 2:
        return mLedgerCloseMeta.v2().ledgerHeader;
#endif
    default:
        releaseAssert(false);
    }
}

void
LedgerCloseMetaFrame::reserveTxProcessing(size_t n)
{
    switch (mVersion)
    {
    case 0:
        mLedgerCloseMeta.v0().txProcessing.reserve(n);
        break;
    case 1:
        mLedgerCloseMeta.v1().txProcessing.reserve(n);
        break;
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    case 2:
        mLedgerCloseMeta.v2().txProcessing.reserve(n);
        break;
#endif
    default:
        releaseAssert(false);
    }
}

void
LedgerCloseMetaFrame::pushTxProcessingEntry()
{
    switch (mVersion)
    {
    case 0:
        mLedgerCloseMeta.v0().txProcessing.emplace_back();
        break;
    case 1:
        mLedgerCloseMeta.v1().txProcessing.emplace_back();
        break;
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    case 2:
        mLedgerCloseMeta.v2().txProcessing.emplace_back();
        break;
#endif
    default:
        releaseAssert(false);
    }
}

void
LedgerCloseMetaFrame::setLastTxProcessingFeeProcessingChanges(
    LedgerEntryChanges const& changes)
{
    switch (mVersion)
    {
    case 0:
        mLedgerCloseMeta.v0().txProcessing.back().feeProcessing = changes;
        break;
    case 1:
        mLedgerCloseMeta.v1().txProcessing.back().feeProcessing = changes;
        break;
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    case 2:
        mLedgerCloseMeta.v2().txProcessing.back().feeProcessing = changes;
        break;
#endif
    default:
        releaseAssert(false);
    }
}

void
LedgerCloseMetaFrame::setTxProcessingMetaAndResultPair(
    TransactionMeta const& tm, TransactionResultPair&& rp, int index)
{
    switch (mVersion)
    {
    case 0:
    {
        auto& txp = mLedgerCloseMeta.v0().txProcessing.at(index);
        txp.txApplyProcessing = tm;
        txp.result = std::move(rp);
    }
    break;
    case 1:
    {
        auto& txp = mLedgerCloseMeta.v1().txProcessing.at(index);
        txp.txApplyProcessing = tm;
        txp.result = std::move(rp);
    }
    break;
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    case 2:
    {
        auto& txp = mLedgerCloseMeta.v2().txProcessing.at(index);
        txp.txApplyProcessing = tm;
        auto& res = txp.result;
        res.transactionHash = std::move(rp.transactionHash);

        // In v2 we do not store the actual txresult anymore, just a hash of
        // hashes. This means we had to get a TransactionMetaV3.
        releaseAssert(tm.v() == 3);
        res.hashOfMetaHashes = TransactionMetaFrame::getHashOfMetaHashes(tm);
    }
    break;
#endif
    default:
        releaseAssert(false);
    }
}

xdr::xvector<UpgradeEntryMeta>&
LedgerCloseMetaFrame::upgradesProcessing()
{
    switch (mVersion)
    {
    case 0:
        return mLedgerCloseMeta.v0().upgradesProcessing;
    case 1:
        return mLedgerCloseMeta.v1().upgradesProcessing;
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    case 2:
        return mLedgerCloseMeta.v2().upgradesProcessing;
#endif
    default:
        releaseAssert(false);
    }
}

void
LedgerCloseMetaFrame::populateTxSet(TxSetFrame const& txSet)
{
    switch (mVersion)
    {
    case 0:
        txSet.toXDR(mLedgerCloseMeta.v0().txSet);
        break;
    case 1:
        txSet.toXDR(mLedgerCloseMeta.v1().txSet);
        break;
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    case 2:
        txSet.toXDR(mLedgerCloseMeta.v2().txSet);
        break;
#endif
    default:
        releaseAssert(false);
    }
}

LedgerCloseMeta const&
LedgerCloseMetaFrame::getXDR() const
{
    return mLedgerCloseMeta;
}
}
