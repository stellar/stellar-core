// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerHeaderReference.h"
#include "xdr/Stellar-ledger.h"
#include <cassert>

namespace stellar
{

LedgerHeaderReference::IgnoreInvalid::IgnoreInvalid(LedgerHeaderReference& lhr)
    : mHeader(lhr.mHeader), mPreviousHeader(lhr.mPreviousHeader)
{
}

LedgerHeader&
LedgerHeaderReference::IgnoreInvalid::header()
{
    return mHeader;
}

LedgerHeader const&
LedgerHeaderReference::IgnoreInvalid::previousHeader()
{
    return mPreviousHeader;
}

LedgerHeaderReference::LedgerHeaderReference(LedgerHeader const& header,
                                             LedgerHeader const& previous)
    : mValid(true), mHeader(header), mPreviousHeader(previous)
{
}

LedgerHeader&
LedgerHeaderReference::header()
{
    assert(valid());
    return mHeader;
}

LedgerHeader const&
LedgerHeaderReference::previousHeader()
{
    assert(valid());
    return mPreviousHeader;
}

bool
LedgerHeaderReference::valid()
{
    return mValid;
}

void
LedgerHeaderReference::invalidate()
{
    mValid = false;
}

LedgerHeaderReference::IgnoreInvalid
LedgerHeaderReference::ignoreInvalid()
{
    return IgnoreInvalid(*this);
}
}
