// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/LedgerEntryIsValid.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerTxn.h"
#include "lib/util/format.h"
#include "main/Application.h"
#include "xdrpp/printer.h"

namespace stellar
{

static bool
signerCompare(Signer const& s1, Signer const& s2)
{
    return s1.key < s2.key;
}

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
LedgerEntryIsValid::checkOnOperationApply(Operation const& operation,
                                          OperationResult const& result,
                                          LedgerTxnDelta const& ltxDelta)
{
    uint32_t currLedgerSeq = ltxDelta.header.current.ledgerSeq;
    if (currLedgerSeq > INT32_MAX)
    {
        return fmt::format("LedgerHeader ledgerSeq ({}) exceeds limits ({})",
                           currLedgerSeq, INT32_MAX);
    }

    auto ver = ltxDelta.header.current.ledgerVersion;
    for (auto const& entryDelta : ltxDelta.entry)
    {
        if (!entryDelta.second.current)
            continue;

        auto s = checkIsValid(*entryDelta.second.current, currLedgerSeq, ver);
        if (!s.empty())
        {
            s += ": ";
            s += xdr::xdr_to_string(*entryDelta.second.current);
            return s;
        }
    }
    return {};
}

template <typename IterType>
std::string
LedgerEntryIsValid::check(IterType iter, IterType const& end,
                          uint32_t ledgerSeq, uint32 version) const
{
    for (; iter != end; ++iter)
    {
        auto s = checkIsValid(iter->current->mEntry, ledgerSeq, version);
        if (!s.empty())
        {
            s += ": ";
            s += xdr::xdr_to_string(iter->current->mEntry);
            return s;
        }
    }
    return {};
}

std::string
LedgerEntryIsValid::checkIsValid(LedgerEntry const& le, uint32_t ledgerSeq,
                                 uint32 version) const
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
        return checkIsValid(le.data.account(), version);
    case TRUSTLINE:
        return checkIsValid(le.data.trustLine(), version);
    case OFFER:
        return checkIsValid(le.data.offer(), version);
    case DATA:
        return checkIsValid(le.data.data(), version);
    default:
        return "LedgerEntry has invalid type";
    }
}

std::string
LedgerEntryIsValid::checkIsValid(AccountEntry const& ae, uint32 version) const
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
                               return !signerCompare(s1, s2);
                           }) != ae.signers.end())
    {
        return "Account signers are not strictly increasing";
    }
    if (version > 9)
    {
        if (!std::all_of(ae.signers.begin(), ae.signers.end(),
                         [](Signer const& s) {
                             return (s.weight <= UINT8_MAX) && (s.weight != 0);
                         }))
        {
            return "Account signers have invalid weights";
        }
    }
    return {};
}

std::string
LedgerEntryIsValid::checkIsValid(TrustLineEntry const& tl, uint32 version) const
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
LedgerEntryIsValid::checkIsValid(OfferEntry const& oe, uint32 version) const
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
LedgerEntryIsValid::checkIsValid(DataEntry const& de, uint32 version) const
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
