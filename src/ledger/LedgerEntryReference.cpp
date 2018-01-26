// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerEntryReference.h"
#include "ledger/LedgerState.h"
#include "util/make_unique.h"
#include "xdr/Stellar-ledger.h"
#include <cassert>

// TODO(jonjove): Remove this header (used for LedgerEntryKey)
#include "ledger/EntryFrame.h"

namespace stellar
{

LedgerEntryReference::IgnoreInvalid::IgnoreInvalid(LedgerEntryReference& ler)
    : mEntry(ler.mEntry), mPreviousEntry(ler.mPreviousEntry)
{
}

std::shared_ptr<LedgerEntry> const&
LedgerEntryReference::IgnoreInvalid::entry()
{
    return mEntry;
}

std::shared_ptr<LedgerEntry const> const&
LedgerEntryReference::IgnoreInvalid::previousEntry()
{
    return mPreviousEntry;
}

LedgerEntryReference::LedgerEntryReference(
    LedgerState* ledgerState, std::shared_ptr<LedgerEntry const> const& entry,
    std::shared_ptr<LedgerEntry const> const& previous)
    : mLedgerState(ledgerState)
    , mEntry(entry ? std::make_shared<LedgerEntry>(*entry) : nullptr)
    , mPreviousEntry(previous ? std::make_shared<LedgerEntry const>(*previous)
                              : nullptr)
{
}

std::shared_ptr<LedgerEntry> const&
LedgerEntryReference::entry()
{
    assert(valid());
    return mEntry;
}

std::shared_ptr<LedgerEntry const> const&
LedgerEntryReference::previousEntry()
{
    assert(valid());
    return mPreviousEntry;
}

void
LedgerEntryReference::erase()
{
    assert(valid());
    mEntry.reset();
}

bool
LedgerEntryReference::valid()
{
    return mLedgerState;
}

void
LedgerEntryReference::invalidate()
{
    mLedgerState = nullptr;
}

void
LedgerEntryReference::forgetFromLedgerState()
{
    assert(valid());
    assert(mEntry);
    mLedgerState->forget(LedgerEntryKey(*mEntry));
    invalidate();
}

LedgerEntryReference::IgnoreInvalid
LedgerEntryReference::ignoreInvalid()
{
    return IgnoreInvalid(*this);
}
}
