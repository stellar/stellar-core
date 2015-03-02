#include "ledger/LedgerMaster.h"
#include "transactions/AllowTrustTxFrame.h"
#include "ledger/TrustFrame.h"
#include "database/Database.h"

namespace stellar
{
    AllowTrustTxFrame::AllowTrustTxFrame(const TransactionEnvelope& envelope) : TransactionFrame(envelope)
    {

    }

    int32_t AllowTrustTxFrame::getNeededThreshold()
    {
        return mSigningAccount->getLowThreshold();
    }

    bool AllowTrustTxFrame::doApply(LedgerDelta &delta, LedgerMaster& ledgerMaster)
    {
        if(!(mSigningAccount->getAccount().flags & AUTH_REQUIRED_FLAG))
        {   // this account doesn't require authorization to hold credit
            innerResult().code(AllowTrust::MALFORMED);
            return false;
        }

        AllowTrustTx const& allowTrust = mEnvelope.tx.body.allowTrustTx();

        Currency ci;
        ci.type(ISO4217);
        ci.isoCI().currencyCode = allowTrust.code.currencyCode();
        ci.isoCI().issuer=mEnvelope.tx.account;

        Database &db = ledgerMaster.getDatabase();
        TrustFrame trustLine;
        if(!TrustFrame::loadTrustLine(allowTrust.trustor, ci, trustLine, db))
        {
            innerResult().code(AllowTrust::NO_TRUST_LINE);
            return false;
        }

        innerResult().code(AllowTrust::SUCCESS);

        trustLine.getTrustLine().authorized = allowTrust.authorize;
        trustLine.storeChange(delta, db);

        return true;
    }

    bool AllowTrustTxFrame::doCheckValid(Application& app)
    {
        if (mEnvelope.tx.body.allowTrustTx().code.type() != ISO4217)
        {
            innerResult().code(AllowTrust::MALFORMED);
            return false;
        }
        return true;
    }
}