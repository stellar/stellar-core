// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/CreateClaimableBalanceOpFrame.h"
#include "crypto/SHA.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/TrustLineWrapper.h"
#include "transactions/TransactionUtils.h"

namespace stellar
{

int64_t
relativeToAbsolute(TimePoint closeTime, int64_t relative)
{
    return closeTime > static_cast<uint64_t>(INT64_MAX - relative)
               ? INT64_MAX
               : closeTime + relative;
}

// convert all relative predicates to absolute predicates
void
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
    case CLAIM_PREDICATE_BEFORE_RELATIVE_TIME:
    {
        auto relBefore = pred.relBefore();
        pred.type(CLAIM_PREDICATE_BEFORE_ABSOLUTE_TIME);
        pred.absBefore() = relativeToAbsolute(closeTime, relBefore);

        break;
    }
    case CLAIM_PREDICATE_AFTER_RELATIVE_TIME:
    {
        auto relAfter = pred.relAfter();
        pred.type(CLAIM_PREDICATE_AFTER_ABSOLUTE_TIME);
        pred.absAfter() = relativeToAbsolute(closeTime, relAfter);

        break;
    }
    case CLAIM_PREDICATE_BEFORE_ABSOLUTE_TIME:
    case CLAIM_PREDICATE_AFTER_ABSOLUTE_TIME:
    case CLAIM_PREDICATE_UNCONDITIONAL:
        break;
    default:
        throw std::runtime_error("Invalid ClaimPredicate");
    }
}

bool
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

    case CLAIM_PREDICATE_BEFORE_ABSOLUTE_TIME:
        return pred.absBefore() >= 0;
    case CLAIM_PREDICATE_AFTER_ABSOLUTE_TIME:
        return pred.absAfter() >= 0;
    case CLAIM_PREDICATE_BEFORE_RELATIVE_TIME:
        return pred.relBefore() >= 0;
    case CLAIM_PREDICATE_AFTER_RELATIVE_TIME:
        return pred.relAfter() >= 0;
    default:
        throw std::runtime_error("Invalid ClaimPredicate");
    }

    return true;
}

CreateClaimableBalanceOpFrame::CreateClaimableBalanceOpFrame(
    Operation const& op, OperationResult& res, TransactionFrame& parentTx,
    uint32_t index)
    : OperationFrame(op, res, parentTx)
    , mCreateClaimableBalance(mOperation.body.createClaimableBalanceOp())
    , mOpIndex(index)
{
}

bool
CreateClaimableBalanceOpFrame::isVersionSupported(
    uint32_t protocolVersion) const
{
    return protocolVersion >= 14;
}

bool
CreateClaimableBalanceOpFrame::doApply(AbstractLedgerTxn& ltx)
{
    auto header = ltx.loadHeader();
    auto sourceAccount = loadSourceAccount(ltx, header);

    auto const& claimants = mCreateClaimableBalance.claimants;

    // Deduct claimants.size() * baseReserve of native asset
    int64_t entryCost =
        int64_t(header.current().baseReserve) * claimants.size();

    if (getAvailableBalance(header, sourceAccount) < entryCost)
    {
        innerResult().code(CREATE_CLAIMABLE_BALANCE_LOW_RESERVE);
        return false;
    }

    auto costOk = addBalance(header, sourceAccount, -entryCost);
    assert(costOk);

    // Deduct amount of asset from sourceAccount
    auto const& asset = mCreateClaimableBalance.asset;
    auto amount = mCreateClaimableBalance.amount;

    if (asset.type() == ASSET_TYPE_NATIVE)
    {
        if (getAvailableBalance(header, sourceAccount) < amount)
        {
            innerResult().code(CREATE_CLAIMABLE_BALANCE_UNDERFUNDED);
            return false;
        }

        auto amountOk = addBalance(header, sourceAccount, -amount);
        assert(amountOk);
    }
    else
    {
        auto trustline = loadTrustLine(ltx, getSourceID(), asset);
        if (!trustline)
        {
            innerResult().code(CREATE_CLAIMABLE_BALANCE_NO_TRUST);
            return false;
        }
        if (!trustline.isAuthorized())
        {
            innerResult().code(CREATE_CLAIMABLE_BALANCE_NOT_AUTHORIZED);
            return false;
        }

        if (!trustline.addBalance(header, -amount))
        {
            innerResult().code(CREATE_CLAIMABLE_BALANCE_UNDERFUNDED);
            return false;
        }
    }

    // Create claimable balance entry
    LedgerEntry newClaimableBalance;
    newClaimableBalance.data.type(CLAIMABLE_BALANCE);

    auto& claimableBalanceEntry = newClaimableBalance.data.claimableBalance();
    claimableBalanceEntry.createdBy = getSourceID();
    claimableBalanceEntry.amount = amount;
    claimableBalanceEntry.asset = asset;
    claimableBalanceEntry.reserve = entryCost;

    OperationID operationID;
    operationID.type(ENVELOPE_TYPE_OP_ID);
    operationID.id().sourceAccount = toMuxedAccount(mParentTx.getSourceID());
    operationID.id().seqNum = mParentTx.getSeqNum();
    operationID.id().opNum = mOpIndex;

    claimableBalanceEntry.balanceID.v0() =
        sha256(xdr::xdr_to_opaque(operationID));

    claimableBalanceEntry.claimants = claimants;
    for (auto& claimant : claimableBalanceEntry.claimants)
    {
        updatePredicatesForApply(claimant.v0().predicate,
                                 header.current().scpValue.closeTime);
    }

    ltx.create(newClaimableBalance);

    innerResult().code(CREATE_CLAIMABLE_BALANCE_SUCCESS);
    innerResult().balanceID() = claimableBalanceEntry.balanceID;
    return true;
}

bool
CreateClaimableBalanceOpFrame::doCheckValid(uint32_t ledgerVersion)
{
    auto const& claimants = mCreateClaimableBalance.claimants;

    if (!isAssetValid(mCreateClaimableBalance.asset) ||
        mCreateClaimableBalance.amount <= 0 || claimants.empty())
    {
        innerResult().code(CREATE_CLAIMABLE_BALANCE_MALFORMED);
        return false;
    }

    // check for duplicates
    std::unordered_set<AccountID> dests;
    for (auto const& claimant : claimants)
    {
        auto const& dest = claimant.v0().destination;
        if (!dests.emplace(dest).second)
        {
            innerResult().code(CREATE_CLAIMABLE_BALANCE_MALFORMED);
            return false;
        }
    }

    for (auto const& claimant : claimants)
    {
        if (!validatePredicate(claimant.v0().predicate, 1))
        {
            innerResult().code(CREATE_CLAIMABLE_BALANCE_MALFORMED);
            return false;
        }
    }

    return true;
}

void
CreateClaimableBalanceOpFrame::insertLedgerKeysToPrefetch(
    std::unordered_set<LedgerKey>& keys) const
{
    // Prefetch trustline for non-native assets
    if (mCreateClaimableBalance.asset.type() != ASSET_TYPE_NATIVE)
    {
        keys.emplace(
            trustlineKey(getSourceID(), mCreateClaimableBalance.asset));
    }
}
}
