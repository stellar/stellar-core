// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/ClawbackOpFrame.h"
#include "ledger/LedgerTxn.h"
#include "transactions/TransactionUtils.h"
#include <Tracy.hpp>

namespace stellar
{

ClawbackOpFrame::ClawbackOpFrame(Operation const& op, OperationResult& res,
                                 TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx), mClawback(mOperation.body.clawbackOp())
{
}

bool
ClawbackOpFrame::isVersionSupported(uint32_t protocolVersion) const
{
    return protocolVersion >= 16;
}

bool
ClawbackOpFrame::doApply(AbstractLedgerTxn& ltx)
{
    ZoneNamedN(applyZone, "ClawbackOp apply", true);

    auto trust =
        ltx.load(trustlineKey(toAccountID(mClawback.from), mClawback.asset));
    if (!trust)
    {
        innerResult().code(CLAWBACK_NO_TRUST);
        return false;
    }

    if (!isClawbackEnabledOnTrustline(trust))
    {
        innerResult().code(CLAWBACK_NOT_CLAWBACK_ENABLED);
        return false;
    }

    auto header = ltx.loadHeader();
    if (!addBalanceSkipAuthorization(header, trust, -mClawback.amount))
    {
        innerResult().code(CLAWBACK_UNDERFUNDED);
        return false;
    }

    innerResult().code(CLAWBACK_SUCCESS);
    return true;
}

bool
ClawbackOpFrame::doCheckValid(uint32_t ledgerVersion)
{
    if (mClawback.from == toMuxedAccount(getSourceID()))
    {
        innerResult().code(CLAWBACK_MALFORMED);
        return false;
    }

    if (mClawback.amount < 1)
    {
        innerResult().code(CLAWBACK_MALFORMED);
        return false;
    }

    if (mClawback.asset.type() == ASSET_TYPE_NATIVE)
    {
        innerResult().code(CLAWBACK_MALFORMED);
        return false;
    }

    if (!isAssetValid(mClawback.asset))
    {
        innerResult().code(CLAWBACK_MALFORMED);
        return false;
    }

    if (!(getSourceID() == getIssuer(mClawback.asset)))
    {
        innerResult().code(CLAWBACK_MALFORMED);
        return false;
    }

    return true;
}

void
ClawbackOpFrame::insertLedgerKeysToPrefetch(UnorderedSet<LedgerKey>& keys) const
{
    if (mClawback.asset.type() != ASSET_TYPE_NATIVE)
    {
        keys.emplace(
            trustlineKey(toAccountID(mClawback.from), mClawback.asset));
    }
}
}
