#include "ledger/LedgerMaster.h"
#include "transactions/AllowTrustTxFrame.h"
#include "ledger/TrustFrame.h"
#include "database/Database.h"

namespace stellar
{
    AllowTrustTxFrame::AllowTrustTxFrame(Operation const& op, OperationResult &res,
        TransactionFrame &parentTx) :
        OperationFrame(op, res, parentTx), mAllowTrust(mOperation.body.allowTrustTx())
    {
    }

    int32_t AllowTrustTxFrame::getNeededThreshold()
    {
        return mSourceAccount->getLowThreshold();
    }

    bool AllowTrustTxFrame::doApply(LedgerDelta &delta, LedgerMaster& ledgerMaster)
    {
        if(!(mSourceAccount->getAccount().flags & AUTH_REQUIRED_FLAG))
        {   // this account doesn't require authorization to hold credit
            innerResult().code(AllowTrust::MALFORMED);
            return false;
        }

        Currency ci;
        ci.type(ISO4217);
        ci.isoCI().currencyCode = mAllowTrust.currency.currencyCode();
        ci.isoCI().issuer = getSourceID();

        Database &db = ledgerMaster.getDatabase();
        TrustFrame trustLine;
        if(!TrustFrame::loadTrustLine(mAllowTrust.trustor, ci, trustLine, db))
        {
            innerResult().code(AllowTrust::NO_TRUST_LINE);
            return false;
        }

        innerResult().code(AllowTrust::SUCCESS);

        trustLine.getTrustLine().authorized = mAllowTrust.authorize;
        trustLine.storeChange(delta, db);

        return true;
    }

    bool AllowTrustTxFrame::doCheckValid(Application& app)
    {
        if (mAllowTrust.currency.type() != ISO4217)
        {
            innerResult().code(AllowTrust::MALFORMED);
            return false;
        }
        return true;
    }
}