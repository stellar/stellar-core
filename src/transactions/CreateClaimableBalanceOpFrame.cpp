// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/CreateClaimableBalanceOpFrame.h"
#include "crypto/SHA.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/TrustLineWrapper.h"
#include "transactions/SponsorshipUtils.h"
#include "transactions/TransactionUtils.h"
#include "util/GlobalChecks.h"
#include "util/ProtocolVersion.h"
#include <Tracy.hpp>

namespace stellar
{

static int64_t
relativeToAbsolute(TimePoint closeTime, int64_t relative)
{
    return closeTime > static_cast<uint64_t>(INT64_MAX - relative)
               ? INT64_MAX
               : closeTime + relative;
}

// convert all relative predicates to absolute predicates
static void
updatePredicatesForApply(ClaimPredicate& pred, TimePoint closeTime)
{
    switch (pred.type())
    {
    case CLAIM_PREDICATE_AND:
    {
        auto& andPredicates = pred.andPredicates();

        updatePredicatesForApply(andPredicates.at(0), closeTime);
        updatePredicatesForApply(andPredicates.at(1), closeTime);

        break;
    }

    case CLAIM_PREDICATE_OR:
    {
        auto& orPredicates = pred.orPredicates();

        updatePredicatesForApply(orPredicates.at(0), closeTime);
        updatePredicatesForApply(orPredicates.at(1), closeTime);

        break;
    }
    case CLAIM_PREDICATE_NOT:
    {
        updatePredicatesForApply(*pred.notPredicate(), closeTime);
        break;
    }
    case CLAIM_PREDICATE_BEFORE_RELATIVE_TIME:
    {
        auto relBefore = pred.relBefore();
        pred.type(CLAIM_PREDICATE_BEFORE_ABSOLUTE_TIME);
        pred.absBefore() = relativeToAbsolute(closeTime, relBefore);

        break;
    }
    case CLAIM_PREDICATE_BEFORE_ABSOLUTE_TIME:
    case CLAIM_PREDICATE_UNCONDITIONAL:
        break;
    default:
        throw std::runtime_error("Invalid ClaimPredicate");
    }
}

static bool
validatePredicate(ClaimPredicate const& pred, uint32_t depth)
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
    case CLAIM_PREDICATE_BEFORE_RELATIVE_TIME:
        return pred.relBefore() >= 0;
    default:
        throw std::runtime_error("Invalid ClaimPredicate");
    }

    return true;
}

CreateClaimableBalanceOpFrame::CreateClaimableBalanceOpFrame(
    Operation const& op, TransactionFrame const& parentTx, uint32_t index)
    : OperationFrame(op, parentTx)
    , mCreateClaimableBalance(mOperation.body.createClaimableBalanceOp())
    , mOpIndex(index)
{
}

bool
CreateClaimableBalanceOpFrame::isOpSupported(LedgerHeader const& header) const
{
    return protocolVersionStartsFrom(header.ledgerVersion,
                                     ProtocolVersion::V_14);
}

bool
CreateClaimableBalanceOpFrame::doApply(
    AppConnector& app, AbstractLedgerTxn& ltx, Hash const& sorobanBasePrngSeed,
    OperationResult& res,
    std::optional<RefundableFeeTracker>& refundableFeeTracker,
    OperationMetaBuilder& opMeta) const
{
    ZoneNamedN(applyZone, "CreateClaimableBalanceOpFrame apply", true);

    auto header = ltx.loadHeader();
    auto sourceAccount = loadSourceAccount(ltx, header);

    auto const& claimants = mCreateClaimableBalance.claimants;

    // Deduct amount of asset from sourceAccount
    auto const& asset = mCreateClaimableBalance.asset;
    auto amount = mCreateClaimableBalance.amount;

    // Create claimable balance entry
    LedgerEntry newClaimableBalance;
    newClaimableBalance.data.type(CLAIMABLE_BALANCE);

    auto& claimableBalanceEntry = newClaimableBalance.data.claimableBalance();

    if (asset.type() == ASSET_TYPE_NATIVE)
    {
        if (getAvailableBalance(header, sourceAccount) < amount)
        {
            innerResult(res).code(CREATE_CLAIMABLE_BALANCE_UNDERFUNDED);
            return false;
        }

        auto amountOk = addBalance(header, sourceAccount, -amount);
        releaseAssertOrThrow(amountOk);
    }
    else
    {
        auto trustline = loadTrustLine(ltx, getSourceID(), asset);
        if (!trustline)
        {
            innerResult(res).code(CREATE_CLAIMABLE_BALANCE_NO_TRUST);
            return false;
        }
        if (!trustline.isAuthorized())
        {
            innerResult(res).code(CREATE_CLAIMABLE_BALANCE_NOT_AUTHORIZED);
            return false;
        }

        if (!trustline.addBalance(header, -amount))
        {
            innerResult(res).code(CREATE_CLAIMABLE_BALANCE_UNDERFUNDED);
            return false;
        }

        if (protocolVersionStartsFrom(header.current().ledgerVersion,
                                      ProtocolVersion::V_17))
        {
            bool enableClawback;
            if (getSourceID() == getIssuer(asset))
            {
                enableClawback = isClawbackEnabledOnAccount(sourceAccount);
            }
            else
            {
                enableClawback = trustline.isClawbackEnabled();
            }

            if (enableClawback)
            {
                setClaimableBalanceClawbackEnabled(claimableBalanceEntry);
            }
        }
    }

    claimableBalanceEntry.amount = amount;
    claimableBalanceEntry.asset = asset;

    claimableBalanceEntry.balanceID.v0() = getBalanceID();

    claimableBalanceEntry.claimants = claimants;
    for (auto& claimant : claimableBalanceEntry.claimants)
    {
        updatePredicatesForApply(claimant.v0().predicate,
                                 header.current().scpValue.closeTime);
    }

    switch (createEntryWithPossibleSponsorship(ltx, header, newClaimableBalance,
                                               sourceAccount))
    {
    case SponsorshipResult::SUCCESS:
        break;
    case SponsorshipResult::LOW_RESERVE:
        innerResult(res).code(CREATE_CLAIMABLE_BALANCE_LOW_RESERVE);
        return false;
    case SponsorshipResult::TOO_MANY_SPONSORING:
        res.code(opTOO_MANY_SPONSORING);
        return false;
    case SponsorshipResult::TOO_MANY_SPONSORED:
        // This is impossible because there's no sponsored account. Fall through
        // and throw.
    case SponsorshipResult::TOO_MANY_SUBENTRIES:
        // This is impossible because claimable balances don't use subentries.
        // Fall through and throw.
    default:
        throw std::runtime_error("Unexpected result from "
                                 "canEstablishEntrySponsorship");
    }

    ltx.create(newClaimableBalance);

    opMeta.getEventManager().eventForTransferWithIssuerCheck(
        asset, makeMuxedAccountAddress(getSourceAccount()),
        makeClaimableBalanceAddress(claimableBalanceEntry.balanceID), amount,
        true);

    innerResult(res).code(CREATE_CLAIMABLE_BALANCE_SUCCESS);
    innerResult(res).balanceID() = claimableBalanceEntry.balanceID;
    return true;
}

bool
CreateClaimableBalanceOpFrame::doCheckValid(uint32_t ledgerVersion,
                                            OperationResult& res) const
{
    auto const& claimants = mCreateClaimableBalance.claimants;

    if (!isAssetValid(mCreateClaimableBalance.asset, ledgerVersion) ||
        mCreateClaimableBalance.amount <= 0 || claimants.empty())
    {
        innerResult(res).code(CREATE_CLAIMABLE_BALANCE_MALFORMED);
        return false;
    }

    // check for duplicates
    UnorderedSet<AccountID> dests;
    for (auto const& claimant : claimants)
    {
        auto const& dest = claimant.v0().destination;
        if (!dests.emplace(dest).second)
        {
            innerResult(res).code(CREATE_CLAIMABLE_BALANCE_MALFORMED);
            return false;
        }
    }

    for (auto const& claimant : claimants)
    {
        if (!validatePredicate(claimant.v0().predicate, 1))
        {
            innerResult(res).code(CREATE_CLAIMABLE_BALANCE_MALFORMED);
            return false;
        }
    }

    return true;
}

void
CreateClaimableBalanceOpFrame::insertLedgerKeysToPrefetch(
    UnorderedSet<LedgerKey>& keys) const
{
    // Prefetch trustline for non-native assets
    if (mCreateClaimableBalance.asset.type() != ASSET_TYPE_NATIVE)
    {
        keys.emplace(
            trustlineKey(getSourceID(), mCreateClaimableBalance.asset));
    }
}

Hash
CreateClaimableBalanceOpFrame::getBalanceID() const
{
    HashIDPreimage hashPreimage;
    hashPreimage.type(ENVELOPE_TYPE_OP_ID);
    hashPreimage.operationID().sourceAccount = mParentTx.getSourceID();
    hashPreimage.operationID().seqNum = mParentTx.getSeqNum();
    hashPreimage.operationID().opNum = mOpIndex;

    return xdrSha256(hashPreimage);
}
}
