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
        if(!(mSigningAccount->mEntry.account().flags & AUTH_REQUIRED_FLAG))
        {   // this account doesn't require authorization to hold credit
            innerResult().result.code(AllowTrust::MALFORMED);
            return false;
        }

        Currency ci;
        ci.type(ISO4217);
        ci.isoCI().currencyCode = mEnvelope.tx.body.allowTrustTx().code.currencyCode();
        ci.isoCI().issuer=mEnvelope.tx.account;

        Database &db = ledgerMaster.getDatabase();
        TrustFrame trustLine;
        if(!TrustFrame::loadTrustLine(mEnvelope.tx.body.allowTrustTx().trustor, ci,
            trustLine, db))
        {
            innerResult().result.code(AllowTrust::NO_TRUST_LINE);
            return false;
        }

        innerResult().result.code(AllowTrust::SUCCESS);

        trustLine.mEntry.trustLine().authorized = mEnvelope.tx.body.allowTrustTx().authorize;
        trustLine.storeChange(delta, db);

        return true;
    }

    bool AllowTrustTxFrame::doCheckValid(Application& app)
    {
        if (mEnvelope.tx.body.allowTrustTx().code.type() != ISO4217)
        {
            innerResult().result.code(AllowTrust::MALFORMED);
            return false;
        }
        return true;
    }
}