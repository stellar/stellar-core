// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerCloseMetaFrame.h"
#include "crypto/SHA.h"
#include "ledger/LedgerTypeUtils.h"
#include "transactions/TransactionMetaFrame.h"
#include "util/GlobalChecks.h"
#include "util/ProtocolVersion.h"
#include "xdr/Stellar-ledger.h"

namespace stellar
{
LedgerCloseMetaFrame::LedgerCloseMetaFrame(uint32_t protocolVersion)
{
    // The LedgerCloseMeta v() switch can be in 2 positions, 0 and 1. We
    // currently support all of these cases, depending on both compile time
    // and runtime conditions.
    mVersion = 0;

    if (protocolVersionStartsFrom(protocolVersion, SOROBAN_PROTOCOL_VERSION))
    {
        mVersion = 1;
    }
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
    default:
        releaseAssert(false);
    }
}

void
LedgerCloseMetaFrame::populateTxSet(TxSetXDRFrame const& txSet)
{
    switch (mVersion)
    {
    case 0:
        txSet.toXDR(mLedgerCloseMeta.v0().txSet);
        break;
    case 1:
        txSet.toXDR(mLedgerCloseMeta.v1().txSet);
        break;
    default:
        releaseAssert(false);
    }
}

void
LedgerCloseMetaFrame::populateEvictedEntries(
    EvictedStateVectors const& evictedState)
{
    releaseAssert(mVersion == 1);
    for (auto const& key : evictedState.deletedKeys)
    {
        releaseAssertOrThrow(isTemporaryEntry(key) || key.type() == TTL);
        mLedgerCloseMeta.v1().evictedTemporaryLedgerKeys.emplace_back(key);
    }
    for (auto const& entry : evictedState.archivedEntries)
    {
        releaseAssertOrThrow(isPersistentEntry(entry.data));
        // Unfortunately, for legacy purposes, evictedTemporaryLedgerKeys is
        // misnamed and stores all evicted keys, both temp and persistent.
        mLedgerCloseMeta.v1().evictedTemporaryLedgerKeys.emplace_back(
            LedgerEntryKey(entry));
    }
}

void
LedgerCloseMetaFrame::setNetworkConfiguration(
    SorobanNetworkConfig const& networkConfig, bool emitExtV1)
{
    releaseAssert(mVersion == 1);
    mLedgerCloseMeta.v1().totalByteSizeOfBucketList =
        networkConfig.getAverageBucketListSize();

    if (emitExtV1)
    {
        mLedgerCloseMeta.v1().ext.v(1);
        auto& ext = mLedgerCloseMeta.v1().ext.v1();
        ext.sorobanFeeWrite1KB = networkConfig.feeWrite1KB();
    }
}

LedgerCloseMeta const&
LedgerCloseMetaFrame::getXDR() const
{
    return mLedgerCloseMeta;
}
}
