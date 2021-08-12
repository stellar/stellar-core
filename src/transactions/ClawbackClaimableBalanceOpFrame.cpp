// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/ClawbackClaimableBalanceOpFrame.h"
#include "ledger/LedgerTxn.h"
#include "transactions/NewSponsorshipUtils.h"
#include "transactions/TransactionUtils.h"
#include <Tracy.hpp>

using namespace stellar::SponsorshipUtils;

namespace stellar
{

ClawbackClaimableBalanceOpFrame::ClawbackClaimableBalanceOpFrame(
    Operation const& op, OperationResult& res, TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mClawbackClaimableBalance(mOperation.body.clawbackClaimableBalanceOp())
{
}

bool
ClawbackClaimableBalanceOpFrame::isVersionSupported(
    uint32_t protocolVersion) const
{
    return protocolVersion >= 17;
}

bool
ClawbackClaimableBalanceOpFrame::doApply(AbstractLedgerTxn& ltx)
{
    ZoneNamedN(applyZone, "ClawbackClaimableBalanceOp apply", true);

    auto claimableBalanceLtxEntry =
        stellar::loadClaimableBalance(ltx, mClawbackClaimableBalance.balanceID);
    if (!claimableBalanceLtxEntry)
    {
        innerResult().code(CLAWBACK_CLAIMABLE_BALANCE_DOES_NOT_EXIST);
        return false;
    }

    // use a lambda so we don't hold a reference to the Asset
    auto asset = [&]() -> Asset const& {
        return claimableBalanceLtxEntry.current().data.claimableBalance().asset;
    };

    if (asset().type() == ASSET_TYPE_NATIVE)
    {
        innerResult().code(CLAWBACK_CLAIMABLE_BALANCE_NOT_ISSUER);
        return false;
    }

    if (!(getSourceID() == getIssuer(asset())))
    {
        innerResult().code(CLAWBACK_CLAIMABLE_BALANCE_NOT_ISSUER);
        return false;
    }

    if (!isClawbackEnabledOnClaimableBalance(
            claimableBalanceLtxEntry.current()))
    {
        innerResult().code(CLAWBACK_CLAIMABLE_BALANCE_NOT_CLAWBACK_ENABLED);
        return false;
    }

    // Need this to ensure that buckets are the same as the original
    // implementation
    auto sourceAccount = loadSourceAccount(ltx, ltx.loadHeader());

    UnownedEntrySponsorable ues(mClawbackClaimableBalance.balanceID);
    ues.erase(ltx);

    innerResult().code(CLAWBACK_CLAIMABLE_BALANCE_SUCCESS);
    return true;
}

bool
ClawbackClaimableBalanceOpFrame::doCheckValid(uint32_t ledgerVersion)
{
    return true;
}

void
ClawbackClaimableBalanceOpFrame::insertLedgerKeysToPrefetch(
    UnorderedSet<LedgerKey>& keys) const
{
    keys.emplace(claimableBalanceKey(mClawbackClaimableBalance.balanceID));
}
}
