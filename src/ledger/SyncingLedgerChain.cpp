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

    return SyncingLedgerChainAddResult::TOO_NEW;
}

LedgerCloseData const&
SyncingLedgerChain::front() const
{
    return mChain.front();
}

LedgerCloseData const&
SyncingLedgerChain::back() const
{
    return mChain.back();
}

bool
SyncingLedgerChain::empty() const
{
    return mChain.empty();
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
