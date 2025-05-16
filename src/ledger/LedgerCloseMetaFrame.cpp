// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerCloseMetaFrame.h"
#include "crypto/SHA.h"
#include "ledger/LedgerTypeUtils.h"
#include "transactions/TransactionMeta.h"
#include "util/GlobalChecks.h"
#include "util/ProtocolVersion.h"
#include "xdr/Stellar-ledger.h"

namespace stellar
{
LedgerCloseMetaFrame::LedgerCloseMetaFrame(uint32_t protocolVersion)
{
    // The LedgerCloseMeta v() switch can be in 3 positions, 0, 1 and 2. We
    // currently support all of these cases, depending on both compile time
    // and runtime conditions.
    mVersion = 0;

    if (protocolVersionStartsFrom(protocolVersion,
                                  PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION))
    {
        mVersion = 2;
    }
    else if (protocolVersionStartsFrom(protocolVersion,
                                       SOROBAN_PROTOCOL_VERSION))
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
    case 2:
        return mLedgerCloseMeta.v2().ledgerHeader;
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
    case 2:
        mLedgerCloseMeta.v2().txProcessing.reserve(n);
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
    case 2:
        mLedgerCloseMeta.v2().txProcessing.emplace_back();
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
    case 2:
        mLedgerCloseMeta.v2().txProcessing.back().feeProcessing = changes;
        break;
    default:
        releaseAssert(false);
    }
}

void
LedgerCloseMetaFrame::setTxProcessingMetaAndResultPair(
    TransactionMeta&& tm, TransactionResultPair&& rp, int index)
{
    switch (mVersion)
    {
    case 0:
    {
        auto& txp = mLedgerCloseMeta.v0().txProcessing.at(index);
        txp.txApplyProcessing = std::move(tm);
        txp.result = std::move(rp);
    }
    break;
    case 1:
    {
        auto& txp = mLedgerCloseMeta.v1().txProcessing.at(index);
        txp.txApplyProcessing = std::move(tm);
        txp.result = std::move(rp);
    }
    break;
    case 2:
    {
        auto& txp = mLedgerCloseMeta.v2().txProcessing.at(index);
        txp.txApplyProcessing = std::move(tm);
        txp.result = std::move(rp);
    }
    break;
    default:
        releaseAssert(false);
    }
}

void
LedgerCloseMetaFrame::setPostTxApplyFeeProcessing(LedgerEntryChanges&& changes,
                                                  int index)
{
    releaseAssert(mVersion == 2);
    mLedgerCloseMeta.v2().txProcessing.at(index).postTxApplyFeeProcessing =
        std::move(changes);
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
    case 2:
        return mLedgerCloseMeta.v2().upgradesProcessing;
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
    case 2:
        txSet.toXDR(mLedgerCloseMeta.v2().txSet);
        break;
    default:
        releaseAssert(false);
    }
}

void
LedgerCloseMetaFrame::populateEvictedEntries(
    EvictedStateVectors const& evictedState)
{
    releaseAssert(mVersion == 1 || mVersion == 2);
    auto& evictedKeys = mVersion == 1 ? mLedgerCloseMeta.v1().evictedKeys
                                      : mLedgerCloseMeta.v2().evictedKeys;

    for (auto const& key : evictedState.deletedKeys)
    {
        releaseAssertOrThrow(isTemporaryEntry(key) || key.type() == TTL);
        evictedKeys.emplace_back(key);
    }
    for (auto const& entry : evictedState.archivedEntries)
    {
        releaseAssertOrThrow(isPersistentEntry(entry.data));
        evictedKeys.emplace_back(LedgerEntryKey(entry));
    }
}

void
LedgerCloseMetaFrame::setNetworkConfiguration(
    SorobanNetworkConfig const& networkConfig, bool emitExtV1)
{
    releaseAssert(mVersion == 1 || mVersion == 2);

    auto& totalByteSizeOfLiveSorobanState =
        mVersion == 1 ? mLedgerCloseMeta.v1().totalByteSizeOfLiveSorobanState
                      : mLedgerCloseMeta.v2().totalByteSizeOfLiveSorobanState;
    totalByteSizeOfLiveSorobanState = networkConfig.getAverageBucketListSize();

    if (emitExtV1)
    {
        auto& ext = mVersion == 1 ? mLedgerCloseMeta.v1().ext
                                  : mLedgerCloseMeta.v2().ext;
        ext.v(1);
        ext.v1().sorobanFeeWrite1KB = networkConfig.feeRent1KB();
    }
}

LedgerCloseMeta const&
LedgerCloseMetaFrame::getXDR() const
{
    return mLedgerCloseMeta;
}

#ifdef BUILD_TESTS
LedgerCloseMetaFrame::LedgerCloseMetaFrame(LedgerCloseMeta const& lm)
    : mLedgerCloseMeta(lm)
{
    mVersion = lm.v();
    if (mVersion == 1)
    {
        // This field should always be empty and never
        // used. The field only exists for legacy
        // reasons.
        releaseAssert(mLedgerCloseMeta.v1().unused.empty());
    }
}

LedgerHeader const&
LedgerCloseMetaFrame::getLedgerHeader() const
{
    switch (mVersion)
    {
    case 0:
        return mLedgerCloseMeta.v0().ledgerHeader.header;
    case 1:
        return mLedgerCloseMeta.v1().ledgerHeader.header;
    case 2:
        return mLedgerCloseMeta.v2().ledgerHeader.header;
    default:
        releaseAssert(false);
    }
}

xdr::xvector<LedgerKey> const&
LedgerCloseMetaFrame::getEvictedKeys() const
{
    switch (mVersion)
    {
    case 1:
        return mLedgerCloseMeta.v1().evictedKeys;
    case 2:
        return mLedgerCloseMeta.v2().evictedKeys;
    default:
        releaseAssert(false);
    }
}

size_t
LedgerCloseMetaFrame::getTransactionResultMetaCount() const
{
    switch (mVersion)
    {
    case 0:
        return mLedgerCloseMeta.v0().txProcessing.size();
    case 1:
        return mLedgerCloseMeta.v1().txProcessing.size();
    case 2:
        return mLedgerCloseMeta.v2().txProcessing.size();
    default:
        releaseAssert(false);
    }
}

TransactionMeta const&
LedgerCloseMetaFrame::getTransactionMeta(size_t index) const
{
    switch (mVersion)
    {
    case 0:
        return mLedgerCloseMeta.v0().txProcessing.at(index).txApplyProcessing;
    case 1:
        return mLedgerCloseMeta.v1().txProcessing.at(index).txApplyProcessing;
    case 2:
        return mLedgerCloseMeta.v2().txProcessing.at(index).txApplyProcessing;
    default:
        releaseAssert(false);
    }
}

LedgerEntryChanges const&
LedgerCloseMetaFrame::getPostTxApplyFeeProcessing(size_t index) const
{
    releaseAssert(mVersion == 2);
    return mLedgerCloseMeta.v2()
        .txProcessing.at(index)
        .postTxApplyFeeProcessing;
}

#endif

}