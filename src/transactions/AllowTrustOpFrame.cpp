// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "transactions/AllowTrustOpFrame.h"
#include "ledger/LedgerMaster.h"
#include "ledger/TrustFrame.h"
#include "database/Database.h"

namespace stellar
{
AllowTrustOpFrame::AllowTrustOpFrame(Operation const& op, OperationResult& res,
                                     TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mAllowTrust(mOperation.body.allowTrustOp())
{
}

int32_t
AllowTrustOpFrame::getNeededThreshold()
{
    return mSourceAccount->getLowThreshold();
}

bool
AllowTrustOpFrame::doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster)
{
    if (!(mSourceAccount->getAccount().flags & AUTH_REQUIRED_FLAG))
    { // this account doesn't require authorization to hold credit
        innerResult().code(AllowTrust::MALFORMED);
        return false;
    }

    Currency ci;
    ci.type(ISO4217);
    ci.isoCI().currencyCode = mAllowTrust.currency.currencyCode();
    ci.isoCI().issuer = getSourceID();

    Database& db = ledgerMaster.getDatabase();
    TrustFrame trustLine;
    if (!TrustFrame::loadTrustLine(mAllowTrust.trustor, ci, trustLine, db))
    {
        innerResult().code(AllowTrust::NO_TRUST_LINE);
        return false;
    }

    innerResult().code(AllowTrust::SUCCESS);

    trustLine.getTrustLine().authorized = mAllowTrust.authorize;
    trustLine.storeChange(delta, db);

    return true;
}

bool
AllowTrustOpFrame::doCheckValid(Application& app)
{
    if (mAllowTrust.currency.type() != ISO4217)
    {
        innerResult().code(AllowTrust::MALFORMED);
        return false;
    }
    return true;
}
}