// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/LedgerEntryIsValid.h"
#include "invariant/InvariantManager.h"
#include "ledger/AccountReference.h"
#include "ledger/LedgerHeaderReference.h"
#include "ledger/LedgerState.h"
#include "lib/util/format.h"
#include "main/Application.h"
#include "xdrpp/printer.h"

namespace stellar
{

LedgerEntryIsValid::LedgerEntryIsValid() : Invariant(false)
{
}

std::shared_ptr<Invariant>
LedgerEntryIsValid::registerInvariant(Application& app)
{
    return app.getInvariantManager().registerInvariant<LedgerEntryIsValid>();
}

std::string
LedgerEntryIsValid::getName() const
{
    return "LedgerEntryIsValid";
}

std::string
LedgerEntryIsValid::checkOnOperationApply(
    Operation const& operation, OperationResult const& result,
    LedgerState const& ls, std::shared_ptr<LedgerHeaderReference const> header)
{
    uint32_t currLedgerSeq = header->header().ledgerSeq;
    if (currLedgerSeq > INT32_MAX)
    {
        return fmt::format("LedgerHeader ledgerSeq ({}) exceeds limits ({})",
                           currLedgerSeq, INT32_MAX);
    }

    return check(ls, currLedgerSeq);
}

std::string
LedgerEntryIsValid::check(LedgerState const& ls, uint32_t ledgerSeq) const
{
    for (auto const& state : ls)
    {
        if (!state.entry())
        {
            continue;
        }

        auto s = checkIsValid(*state.entry(), ledgerSeq);
        if (!s.empty())
        {
            s += ": ";
            s += xdr::xdr_to_string(*state.entry());
            return s;
        }
    }
    return {};
}

std::string
LedgerEntryIsValid::checkIsValid(LedgerEntry const& le,
                                 uint32_t ledgerSeq) const
{
    if (le.lastModifiedLedgerSeq != ledgerSeq)
    {
        return fmt::format("LedgerEntry lastModifiedLedgerSeq ({}) does not"
                           " equal LedgerHeader ledgerSeq ({})",
                           le.lastModifiedLedgerSeq, ledgerSeq);
    }
    switch (le.data.type())
    {
    case ACCOUNT:
        return checkIsValid(le.data.account());
    case TRUSTLINE:
        return checkIsValid(le.data.trustLine());
    case OFFER:
        return checkIsValid(le.data.offer());
    case DATA:
        return checkIsValid(le.data.data());
    default:
        return "LedgerEntry has invalid type";
    }
}

std::string
LedgerEntryIsValid::checkIsValid(AccountEntry const& ae) const
{
    if (ae.balance < 0)
    {
        return fmt::format("Account balance ({}) is negative", ae.balance);
    }
    if (ae.seqNum < 0)
    {
        return fmt::format("Account seqNum ({}) is negative", ae.seqNum);
    }
    if (ae.numSubEntries > INT32_MAX)
    {
        return fmt::format("Account numSubEntries ({}) exceeds limit ({})",
                           ae.numSubEntries, INT32_MAX);
    }
    if ((ae.flags & ~MASK_ACCOUNT_FLAGS) != 0)
    {
        return "Account flags are invalid";
    }
    if (!isString32Valid(ae.homeDomain))
    {
        return "Account homeDomain is invalid";
    }
    if (std::adjacent_find(ae.signers.begin(), ae.signers.end(),
                           [](Signer const& s1, Signer const& s2) {
                               return !AccountReference::signerCompare(s1, s2);
                           }) != ae.signers.end())
    {
        return "Account signers are not strictly increasing";
    }
    if (!std::all_of(ae.signers.begin(), ae.signers.end(), [](Signer const& s) {
            return (s.weight <= UINT8_MAX) && (s.weight != 0);
        }))
    {
        return "Account signers have invalid weights";
    }
    return {};
}

std::string
LedgerEntryIsValid::checkIsValid(TrustLineEntry const& tl) const
{
    if (tl.asset.type() == ASSET_TYPE_NATIVE)
    {
        return "TrustLine asset is native";
    }
    if (!isAssetValid(tl.asset))
    {
        return "TrustLine asset is invalid";
    }
    if (tl.balance < 0)
    {
        return fmt::format("TrustLine balance ({}) is negative", tl.balance);
    }
    if (tl.limit <= 0)
    {
        return fmt::format("TrustLine balance ({}) is not positive", tl.limit);
    }
    if (tl.balance > tl.limit)
    {
        return fmt::format("TrustLine balance ({}) exceeds limit ({})",
                           tl.balance, tl.limit);
    }
    if ((tl.flags & ~MASK_TRUSTLINE_FLAGS) != 0)
    {
        return "TrustLine flags are invalid";
    }
    return {};
}

std::string
LedgerEntryIsValid::checkIsValid(OfferEntry const& oe) const
{
    if (oe.offerID > INT64_MAX)
    {
        return fmt::format("Offer offerID ({}) exceeds limit ({})", oe.offerID,
                           INT64_MAX);
    }
    if (!isAssetValid(oe.selling))
    {
        return "Offer selling asset is invalid";
    }
    if (!isAssetValid(oe.buying))
    {
        return "Offer buying asset is invalid";
    }
    if (oe.amount <= 0)
    {
        return "Offer amount is not positive";
    }
    if (oe.price.n <= 0 || oe.price.d < 1)
    {
        return fmt::format("Offer price ({} / {}) is invalid", oe.price.n,
                           oe.price.d);
    }
    if ((oe.flags & ~MASK_OFFERENTRY_FLAGS) != 0)
    {
        return "Offer flags are invalid";
    }
    return {};
}

std::string
LedgerEntryIsValid::checkIsValid(DataEntry const& de) const
{
    if (de.dataName.size() == 0)
    {
        return "Data dataName is empty";
    }
    if (!isString32Valid(de.dataName))
    {
        return "Data dataName is invalid";
    }
    return {};
}
}
