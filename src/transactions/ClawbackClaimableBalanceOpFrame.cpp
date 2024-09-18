// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/ClawbackClaimableBalanceOpFrame.h"
#include "ledger/LedgerTxn.h"
#include "transactions/SponsorshipUtils.h"
#include "transactions/TransactionUtils.h"
#include "util/ProtocolVersion.h"
#include <Tracy.hpp>

namespace stellar
{

ClawbackClaimableBalanceOpFrame::ClawbackClaimableBalanceOpFrame(
    Operation const& op, TransactionFrame const& parentTx)
    : OperationFrame(op, parentTx)
    , mClawbackClaimableBalance(mOperation.body.clawbackClaimableBalanceOp())
{
}

bool
ClawbackClaimableBalanceOpFrame::isOpSupported(LedgerHeader const& header) const
{
    return protocolVersionStartsFrom(header.ledgerVersion,
                                     ProtocolVersion::V_17);
}

bool
ClawbackClaimableBalanceOpFrame::doApply(
    AppConnector& app, AbstractLedgerTxn& ltx, Hash const& sorobanBasePrngSeed,
    OperationResult& res, std::shared_ptr<SorobanTxData> sorobanData) const
{
    ZoneNamedN(applyZone, "ClawbackClaimableBalanceOp apply", true);

    auto claimableBalanceLtxEntry =
        stellar::loadClaimableBalance(ltx, mClawbackClaimableBalance.balanceID);
    if (!claimableBalanceLtxEntry)
    {
        innerResult(res).code(CLAWBACK_CLAIMABLE_BALANCE_DOES_NOT_EXIST);
        return false;
    }

    // use a lambda so we don't hold a reference to the Asset
    auto asset = [&]() -> Asset const& {
        return claimableBalanceLtxEntry.current().data.claimableBalance().asset;
    };

    if (asset().type() == ASSET_TYPE_NATIVE)
    {
        innerResult(res).code(CLAWBACK_CLAIMABLE_BALANCE_NOT_ISSUER);
        return false;
    }

    if (!(getSourceID() == getIssuer(asset())))
    {
        innerResult(res).code(CLAWBACK_CLAIMABLE_BALANCE_NOT_ISSUER);
        return false;
    }

    if (!isClawbackEnabledOnClaimableBalance(
            claimableBalanceLtxEntry.current()))
    {
        innerResult(res).code(CLAWBACK_CLAIMABLE_BALANCE_NOT_CLAWBACK_ENABLED);
        return false;
    }

    auto header = ltx.loadHeader();
    auto sourceAccount = loadSourceAccount(ltx, header);
    removeEntryWithPossibleSponsorship(
        ltx, header, claimableBalanceLtxEntry.current(), sourceAccount);

    claimableBalanceLtxEntry.erase();

    innerResult(res).code(CLAWBACK_CLAIMABLE_BALANCE_SUCCESS);
    return true;
}

bool
ClawbackClaimableBalanceOpFrame::doCheckValid(uint32_t ledgerVersion,
                                              OperationResult& res) const
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
