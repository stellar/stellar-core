// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/SponsorshipCountIsValid.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerTxn.h"
#include "main/Application.h"
#include "transactions/TransactionUtils.h"
#include "util/UnorderedMap.h"
#include <fmt/format.h>

namespace stellar
{

SponsorshipCountIsValid::SponsorshipCountIsValid() : Invariant(false)
{
}

std::shared_ptr<Invariant>
SponsorshipCountIsValid::registerInvariant(Application& app)
{
    return app.getInvariantManager()
        .registerInvariant<SponsorshipCountIsValid>();
}

std::string
SponsorshipCountIsValid::getName() const
{
    return "SponsorshipCountIsValid";
}

static int64_t
getMult(LedgerEntry const& le)
{
    switch (le.data.type())
    {
    case ACCOUNT:
        return 2;
    case TRUSTLINE:
    case OFFER:
    case DATA:
        return 1;
    case CLAIMABLE_BALANCE:
        return le.data.claimableBalance().claimants.size();
    default:
        abort();
    }
}

static AccountID const&
getAccountID(LedgerEntry const& le)
{
    switch (le.data.type())
    {
    case ACCOUNT:
        return le.data.account().accountID;
    case TRUSTLINE:
        return le.data.trustLine().accountID;
    case OFFER:
        return le.data.offer().sellerID;
    case DATA:
        return le.data.data().accountID;
    case CLAIMABLE_BALANCE:
    default:
        abort();
    }
}

static void
updateCounters(LedgerEntry const& le,
               UnorderedMap<AccountID, int64_t>& numSponsoring,
               UnorderedMap<AccountID, int64_t>& numSponsored,
               int64_t& claimableBalanceReserve, int64_t sign)
{
    if (le.ext.v() == 1 && le.ext.v1().sponsoringID)
    {
        int64_t mult = sign * getMult(le);
        numSponsoring[*le.ext.v1().sponsoringID] += mult;
        if (le.data.type() != CLAIMABLE_BALANCE)
        {
            numSponsored[getAccountID(le)] += mult;
        }
        else
        {
            claimableBalanceReserve += mult;
        }
    }

    if (le.data.type() == ACCOUNT)
    {
        auto const& ae = le.data.account();
        if (hasAccountEntryExtV2(ae))
        {
            auto const& extV2 = ae.ext.v1().ext.v2();
            for (auto const& s : extV2.signerSponsoringIDs)
            {
                if (s)
                {
                    numSponsoring[*s] += sign;
                    numSponsored[le.data.account().accountID] += sign;
                }
            }
        }
    }
}

static void
updateChangedSponsorshipCounts(
    std::shared_ptr<InternalLedgerEntry const> current,
    std::shared_ptr<InternalLedgerEntry const> previous,
    UnorderedMap<AccountID, int64_t>& numSponsoring,
    UnorderedMap<AccountID, int64_t>& numSponsored,
    int64_t& claimableBalanceReserve)
{
    if (current && current->type() == InternalLedgerEntryType::LEDGER_ENTRY)
    {
        updateCounters(current->ledgerEntry(), numSponsoring, numSponsored,
                       claimableBalanceReserve, 1);
    }
    if (previous && previous->type() == InternalLedgerEntryType::LEDGER_ENTRY)
    {
        updateCounters(previous->ledgerEntry(), numSponsoring, numSponsored,
                       claimableBalanceReserve, -1);
    }
}

static void
getDeltaSponsoringAndSponsored(std::shared_ptr<InternalLedgerEntry const> le,
                               int64_t& numSponsoring, int64_t& numSponsored,
                               int64_t sign)
{
    if (le)
    {
        assert(le->type() == InternalLedgerEntryType::LEDGER_ENTRY &&
               le->ledgerEntry().data.type() == ACCOUNT);
        auto const& ae = le->ledgerEntry().data.account();
        if (hasAccountEntryExtV2(ae))
        {
            auto const& extV2 = ae.ext.v1().ext.v2();
            numSponsoring += sign * extV2.numSponsoring;
            numSponsored += sign * extV2.numSponsored;
        }
    }
}

std::string
SponsorshipCountIsValid::checkOnOperationApply(Operation const& operation,
                                               OperationResult const& result,
                                               LedgerTxnDelta const& ltxDelta)
{
    // No sponsorships prior to protocol 14
    auto ledgerVersion = ltxDelta.header.current.ledgerVersion;
    if (ledgerVersion < 14)
    {
        return {};
    }

    // Get changes in numSponsoring and numSponsored based on extensions
    UnorderedMap<AccountID, int64_t> numSponsoring;
    UnorderedMap<AccountID, int64_t> numSponsored;
    int64_t claimableBalanceReserve = 0;
    for (auto const& kv : ltxDelta.entry)
    {
        updateChangedSponsorshipCounts(kv.second.current, kv.second.previous,
                                       numSponsoring, numSponsored,
                                       claimableBalanceReserve);
    }

    // Check that changes match per account
    for (auto const& kv : ltxDelta.entry)
    {
        if (kv.first.type() != InternalLedgerEntryType::LEDGER_ENTRY ||
            kv.first.ledgerKey().type() != ACCOUNT)
        {
            continue;
        }

        int64_t deltaNumSponsoring = 0;
        int64_t deltaNumSponsored = 0;
        getDeltaSponsoringAndSponsored(kv.second.current, deltaNumSponsoring,
                                       deltaNumSponsored, 1);
        getDeltaSponsoringAndSponsored(kv.second.previous, deltaNumSponsoring,
                                       deltaNumSponsored, -1);

        auto const& accountID = kv.first.ledgerKey().account().accountID;
        if (numSponsoring[accountID] != deltaNumSponsoring)
        {
            return fmt::format(
                "Change in Account {} numSponsoring ({}) does not"
                " match change in number of sponsored entries ({})",
                KeyUtils::toStrKey(accountID), deltaNumSponsoring,
                numSponsoring[accountID]);
        }
        if (numSponsored[accountID] != deltaNumSponsored)
        {
            return fmt::format(
                "Change in Account {} numSponsored ({}) does not"
                " match change in number of sponsored entries ({})",
                KeyUtils::toStrKey(accountID), deltaNumSponsored,
                numSponsored[accountID]);
        }

        numSponsoring.erase(accountID);
        numSponsored.erase(accountID);
    }

    // Make sure we didn't miss any changes
    for (auto const& kv : numSponsoring)
    {
        if (kv.second != 0)
        {
            return fmt::format(
                "Change in Account {} numSponsoring (0) does not"
                " match change in number of sponsored entries ({})",
                KeyUtils::toStrKey(kv.first), kv.second);
        }
    }
    for (auto const& kv : numSponsored)
    {
        if (kv.second != 0)
        {
            return fmt::format(
                "Change in Account {} numSponsored (0) does not"
                " match change in number of sponsored entries ({})",
                KeyUtils::toStrKey(kv.first), kv.second);
        }
    }

    return {};
}
}
