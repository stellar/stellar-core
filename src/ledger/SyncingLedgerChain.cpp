// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/SyncingLedgerChain.h"
#include "herder/LedgerCloseData.h"

namespace stellar
{

SyncingLedgerChain::SyncingLedgerChain() = default;
SyncingLedgerChain::SyncingLedgerChain(SyncingLedgerChain const&) = default;
SyncingLedgerChain::~SyncingLedgerChain() = default;

SyncingLedgerChainAddResult
SyncingLedgerChain::add(LedgerCloseData lcd)
{
    if (mChain.empty() ||
        mChain.back().getLedgerSeq() + 1 == lcd.getLedgerSeq())
    {
        mChain.emplace_back(std::move(lcd));
        return SyncingLedgerChainAddResult::CONTIGUOUS;
    }

    if (lcd.getLedgerSeq() <= mChain.back().getLedgerSeq())
    {
        return SyncingLedgerChainAddResult::TOO_OLD;
    }

    mHadTooNew = true;
    return SyncingLedgerChainAddResult::TOO_NEW;
}

HistoryManager::VerifyHashStatus
SyncingLedgerChain::verifyCatchupCandidate(
    LedgerHeaderHistoryEntry const& candidate) const
{
    if (mChain.empty())
    {
        return HistoryManager::VERIFY_HASH_UNKNOWN_RECOVERABLE;
    }

    auto lookFor = candidate.header.ledgerSeq + 1;
    if (lookFor < mChain.front().getLedgerSeq() || lookFor > mChain.back().getLedgerSeq())
    {
        if (mHadTooNew)
        {
            return HistoryManager::VERIFY_HASH_UNKNOWN_UNRECOVERABLE;
        }
        else
        {
            return HistoryManager::VERIFY_HASH_UNKNOWN_RECOVERABLE;
        }
    }

    auto index = lookFor - mChain.front().getLedgerSeq();
    assert(index >= 0 && index < mChain.size());
    auto lcd = mChain[index];
    assert(lcd.getLedgerSeq() == lookFor);
    if (lcd.getTxSet()->previousLedgerHash() == candidate.hash)
    {
        return HistoryManager::VERIFY_HASH_OK;
    }
    else
    {
        return HistoryManager::VERIFY_HASH_BAD;
    }
}

LedgerCloseData const&
SyncingLedgerChain::back() const
{
    return mChain.back();
}

SyncingLedgerChain::size_type
SyncingLedgerChain::size() const
{
    return mChain.size();
}

SyncingLedgerChain::const_iterator
SyncingLedgerChain::begin() const
{
    return std::begin(mChain);
}

SyncingLedgerChain::const_iterator
SyncingLedgerChain::end() const
{
    return std::end(mChain);
}
}
