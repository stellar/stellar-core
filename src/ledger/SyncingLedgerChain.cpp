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

void
SyncingLedgerChain::pop()
{
    mChain.pop();
}

SyncingLedgerChainAddResult
SyncingLedgerChain::push(LedgerCloseData lcd)
{
    if (mChain.empty() ||
        mChain.back().getLedgerSeq() + 1 == lcd.getLedgerSeq())
    {
        mChain.emplace(lcd);
        return SyncingLedgerChainAddResult::CONTIGUOUS;
    }

    if (lcd.getLedgerSeq() <= mChain.back().getLedgerSeq())
    {
        return SyncingLedgerChainAddResult::TOO_OLD;
    }

    return SyncingLedgerChainAddResult::TOO_NEW;
}

size_t
SyncingLedgerChain::size() const
{
    return mChain.size();
}

bool
SyncingLedgerChain::empty() const
{
    return mChain.empty();
}
}
