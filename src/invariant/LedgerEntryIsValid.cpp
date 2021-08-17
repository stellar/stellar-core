// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/LedgerEntryIsValid.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerTxn.h"
#include "main/Application.h"
#include "transactions/TransactionUtils.h"
#include "util/GlobalChecks.h"
#include "xdrpp/printer.h"
#include <fmt/format.h>

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

        auto s = checkIsValid(*entryDelta.second.current,
                              entryDelta.second.previous, currLedgerSeq, ver);
        if (!s.empty())
        {
            s += ": " + entryDelta.second.current->toString();
            return s;
        }
    }
    return {};
}

std::string
LedgerEntryIsValid::checkIsValid(
    InternalLedgerEntry const& le,
    std::shared_ptr<InternalLedgerEntry const> const& genPrevious,
    uint32_t ledgerSeq, uint32 version) const
{
    if (le.type() == InternalLedgerEntryType::LEDGER_ENTRY)
    {
        auto const* previous =
            genPrevious ? &genPrevious->ledgerEntry() : nullptr;
        return checkIsValid(le.ledgerEntry(), previous, ledgerSeq, version);
    }
    return "";
}

std::string
LedgerEntryIsValid::checkIsValid(LedgerEntry const& le,
                                 LedgerEntry const* previous,
                                 uint32_t ledgerSeq, uint32 version) const
{
    if (le.lastModifiedLedgerSeq != ledgerSeq)
    {
        return fmt::format("LedgerEntry lastModifiedLedgerSeq ({}) does not"
                           " equal LedgerHeader ledgerSeq ({})",
                           le.lastModifiedLedgerSeq, ledgerSeq);
    }

    if (version < 14 && le.ext.v() == 1)
    {
        return "LedgerEntry has v1 extension before protocol version 14";
    }

    switch (le.data.type())
    {
    case ACCOUNT:
        return checkIsValid(le.data.account(), version);
    case TRUSTLINE:
        return checkIsValid(le.data.trustLine(), previous, version);
    case OFFER:
        return checkIsValid(le.data.offer(), version);
    case DATA:
        return checkIsValid(le.data.data(), version);
    case CLAIMABLE_BALANCE:
        if (le.ext.v() != 1 || !le.ext.v1().sponsoringID)
        {
            return "ClaimableBalance is not sponsored";
        }
        return checkIsValid(le.data.claimableBalance(), previous, version);
    case LIQUIDITY_POOL:
        if (le.ext.v() != 0)
        {
            return "LiquidityPool is sponsored";
        }
        return checkIsValid(le.data.liquidityPool(), previous, version);
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

    if (!accountFlagIsValid(ae.flags, version))
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

    if (hasAccountEntryExtV2(ae))
    {
        if (version < 14)
        {
            return "Account has v2 extension before protocol version 14";
        }
        auto const& extV2 = ae.ext.v1().ext.v2();
        if (ae.signers.size() != extV2.signerSponsoringIDs.size())
        {
            return "Account signers not paired with signerSponsoringIDs";
        }
    }
    return {};
}

std::string
LedgerEntryIsValid::checkIsValid(TrustLineEntry const& tl,
                                 LedgerEntry const* previous,
                                 uint32 version) const
{
    if (tl.asset.type() == ASSET_TYPE_NATIVE)
    {
        return "TrustLine asset is native";
    }
    if (!isAssetValid(tl.asset, version))
    {
        return "TrustLine asset is invalid";
    }
    if (tl.asset.type() == ASSET_TYPE_POOL_SHARE && tl.ext.v() == 1 &&
        (tl.ext.v1().liabilities.buying != 0 ||
         tl.ext.v1().liabilities.selling != 0))
    {
        return "Pool share TrustLine has liabilities";
    }
    if (hasTrustLineEntryExtV2(tl))
    {
        if (version < 18)
        {
            return "TrustLine has v2 extension before protocol version 18";
        }
        if (tl.ext.v1().ext.v2().liquidityPoolUseCount < 0)
        {
            return "TrustLine liquidityPoolUseCount is negative";
        }
    }
    if (tl.balance < 0)
    {
        return fmt::format("TrustLine balance ({}) is negative", tl.balance);
    }
    if (tl.limit <= 0)
    {
        return fmt::format("TrustLine limit ({}) is not positive", tl.limit);
    }
    if (tl.balance > tl.limit)
    {
        return fmt::format("TrustLine balance ({}) exceeds limit ({})",
                           tl.balance, tl.limit);
    }
    if (!trustLineFlagIsValid(tl.flags, version))
    {
        return "TrustLine flags are invalid";
    }
    if (previous && !isClawbackEnabledOnTrustline(previous->data.trustLine()) &&
        isClawbackEnabledOnTrustline(tl))
    {
        return "TrustLine clawback flag was enabled";
    }
    return {};
}

std::string
LedgerEntryIsValid::checkIsValid(OfferEntry const& oe, uint32 version) const
{
    if (oe.offerID <= 0)
    {
        return fmt::format("Offer offerID ({}) must be positive", oe.offerID);
    }
    if (!isAssetValid(oe.selling, version))
    {
        return "Offer selling asset is invalid";
    }
    if (!isAssetValid(oe.buying, version))
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

bool
LedgerEntryIsValid::validatePredicate(ClaimPredicate const& pred,
                                      uint32_t depth) const
{
    if (depth > 4)
    {
        return false;
    }

    switch (pred.type())
    {
    case CLAIM_PREDICATE_UNCONDITIONAL:
        break;
    case CLAIM_PREDICATE_AND:
    {
        auto const& andPredicates = pred.andPredicates();
        if (andPredicates.size() != 2)
        {
            return false;
        }
        return validatePredicate(andPredicates[0], depth + 1) &&
               validatePredicate(andPredicates[1], depth + 1);
    }

    case CLAIM_PREDICATE_OR:
    {
        auto const& orPredicates = pred.orPredicates();
        if (orPredicates.size() != 2)
        {
            return false;
        }
        return validatePredicate(orPredicates[0], depth + 1) &&
               validatePredicate(orPredicates[1], depth + 1);
    }
    case CLAIM_PREDICATE_NOT:
    {
        if (!pred.notPredicate())
        {
            return false;
        }
        return validatePredicate(*pred.notPredicate(), depth + 1);
    }

    case CLAIM_PREDICATE_BEFORE_ABSOLUTE_TIME:
        return pred.absBefore() >= 0;
    default:
        return false;
    }

    return true;
}

std::string
LedgerEntryIsValid::checkIsValid(ClaimableBalanceEntry const& cbe,
                                 LedgerEntry const* previous,
                                 uint32 version) const
{
    if (version < 17 && cbe.ext.v() == 1)
    {
        return "ClaimableBalance has v1 extension before protocol version 17";
    }

    if (isClawbackEnabledOnClaimableBalance(cbe) &&
        cbe.asset.type() == ASSET_TYPE_NATIVE)
    {
        return "ClaimableBalance clawback set on native balance";
    }

    if (!claimableBalanceFlagIsValid(cbe))
    {
        return "ClaimableBalance flags are invalid";
    }

    if (previous)
    {
        releaseAssert(previous->data.type() == CLAIMABLE_BALANCE);
        auto const& previousCbe = previous->data.claimableBalance();

        if (!(cbe == previousCbe))
        {
            return "ClaimableBalance cannot be modified";
        }
    }

    if (cbe.claimants.empty())
    {
        return "ClaimableBalance claimants is empty";
    }
    if (!isAssetValid(cbe.asset, version))
    {
        return "ClaimableBalance asset is invalid";
    }
    if (cbe.amount <= 0)
    {
        return "ClaimableBalance amount is not positive";
    }

    for (auto const& claimant : cbe.claimants)
    {
        if (!validatePredicate(claimant.v0().predicate, 1))
        {
            return "ClaimableBalance claimant is invalid";
        }
    }

    return {};
}

std::string
LedgerEntryIsValid::checkIsValid(LiquidityPoolEntry const& lp,
                                 LedgerEntry const* previous,
                                 uint32 version) const
{
    if (version < 18)
    {
        return "LiquidityPools are only valid from V18";
    }

    if (lp.body.type() != LIQUIDITY_POOL_CONSTANT_PRODUCT)
    {
        return "LiquidityPool type must be constant product";
    }

    auto const& cp = lp.body.constantProduct();
    if (!isAssetValid(cp.params.assetA, version))
    {
        return "LiquidityPool assetA is invalid";
    }
    if (!isAssetValid(cp.params.assetB, version))
    {
        return "LiquidityPool assetB is invalid";
    }
    if (!(cp.params.assetA < cp.params.assetB))
    {
        return "LiquidityPool assets are in an invalid order";
    }
    if (cp.params.fee != 30)
    {
        return "LiquidityPool fee is not 30 basis points";
    }

    if (cp.reserveA < 0)
    {
        return "LiquidityPool reserveA is negative";
    }
    if (cp.reserveB < 0)
    {
        return "LiquidityPool reserveB is negative";
    }
    if (cp.totalPoolShares < 0)
    {
        return "LiquidityPool totalPoolShares is negative";
    }
    if (cp.poolSharesTrustLineCount < 0)
    {
        return "LiquidityPool poolSharesTrustLineCount is negative";
    }

    if (previous)
    {
        if (previous->data.type() != LIQUIDITY_POOL)
        {
            return "LiquidityPool used to be of different type";
        }

        auto const& lpPrev = previous->data.liquidityPool();
        if (lpPrev.body.type() != lp.body.type())
        {
            return "LiquidityPool body changed type";
        }

        if (!(lpPrev.body.constantProduct().params ==
              lp.body.constantProduct().params))
        {
            return "LiquidityPool parameters changed";
        }
    }

    return {};
}
}
