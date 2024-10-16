// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/ClawbackOpFrame.h"
#include "ledger/LedgerTxn.h"
#include "transactions/TransactionUtils.h"
#include "util/ProtocolVersion.h"
#include <Tracy.hpp>

namespace stellar
{

ClawbackOpFrame::ClawbackOpFrame(Operation const& op,
                                 TransactionFrame const& parentTx)
    : OperationFrame(op, parentTx), mClawback(mOperation.body.clawbackOp())
{
}

bool
ClawbackOpFrame::isOpSupported(LedgerHeader const& header) const
{
    return protocolVersionStartsFrom(header.ledgerVersion,
                                     ProtocolVersion::V_17);
}

bool
ClawbackOpFrame::doApply(AppConnector& app, AbstractLedgerTxn& ltx,
                         Hash const& sorobanBasePrngSeed, OperationResult& res,
                         std::shared_ptr<SorobanTxData> sorobanData) const
{
    ZoneNamedN(applyZone, "ClawbackOp apply", true);

    auto trust =
        ltx.load(trustlineKey(toAccountID(mClawback.from), mClawback.asset));
    if (!trust)
    {
        innerResult(res).code(CLAWBACK_NO_TRUST);
        return false;
    }

    if (!isClawbackEnabledOnTrustline(trust))
    {
        innerResult(res).code(CLAWBACK_NOT_CLAWBACK_ENABLED);
        return false;
    }

    auto header = ltx.loadHeader();
    if (!addBalanceSkipAuthorization(header, trust, -mClawback.amount))
    {
        innerResult(res).code(CLAWBACK_UNDERFUNDED);
        return false;
    }

    innerResult(res).code(CLAWBACK_SUCCESS);
    return true;
}

bool
ClawbackOpFrame::doCheckValid(uint32_t ledgerVersion,
                              OperationResult& res) const
{
    if (mClawback.from == toMuxedAccount(getSourceID()))
    {
        innerResult(res).code(CLAWBACK_MALFORMED);
        return false;
    }

    if (mClawback.amount < 1)
    {
        innerResult(res).code(CLAWBACK_MALFORMED);
        return false;
    }

    if (mClawback.asset.type() == ASSET_TYPE_NATIVE)
    {
        innerResult(res).code(CLAWBACK_MALFORMED);
        return false;
    }

    if (!isAssetValid(mClawback.asset, ledgerVersion))
    {
        innerResult(res).code(CLAWBACK_MALFORMED);
        return false;
    }

    if (!(getSourceID() == getIssuer(mClawback.asset)))
    {
        innerResult(res).code(CLAWBACK_MALFORMED);
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
