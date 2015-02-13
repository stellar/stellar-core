// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "ledger/EntryFrame.h"
#include "LedgerMaster.h"
#include "ledger/AccountFrame.h"
#include "ledger/OfferFrame.h"
#include "ledger/TrustFrame.h"

namespace stellar
{


EntryFrame::pointer EntryFrame::FromXDR(LedgerEntry const &from)
{
    EntryFrame::pointer res;

    switch (from.type())
    {
    case ACCOUNT:
        res = std::make_shared<AccountFrame>(from);
        break;
    case TRUSTLINE:
        res = std::make_shared<TrustFrame>(from);
        break;
    case OFFER:
        res = std::make_shared<OfferFrame>(from);
        break;
    }
    return res;
}

EntryFrame::EntryFrame()
{
}

EntryFrame::EntryFrame(const LedgerEntry& from) : mEntry(from)
{
}

uint256 EntryFrame::getIndex()
{
    if(isZero(mIndex))
    {
        calculateIndex();
    }
    return mIndex;
}

}
