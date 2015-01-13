#include "transactions/MergeFrame.h"
#include "crypto/Base58.h"
#include "database/Database.h"

using namespace soci;

namespace stellar
{

    MergeFrame::MergeFrame(const TransactionEnvelope& envelope) : TransactionFrame(envelope)
    {

    }

    // make sure the deleted Account hasn't issued credit
    // make sure we aren't holding any credit
    // make sure the we delete all the offers
    // make sure the we delete all the trustlines
    // move the STR to the new account
    bool MergeFrame::doApply(TxDelta& delta, LedgerMaster& ledgerMaster)
    {
        AccountFrame otherAccount;
        if(!ledgerMaster.getDatabase().loadAccount(mEnvelope.tx.body.destination(),otherAccount))
        {
            mResultCode = txNOACCOUNT;
            return false;
        }

        std::string b58Account = toBase58Check(VER_ACCOUNT_ID, mSigningAccount->getID());
        ledgerMaster.getDatabase().getSession() <<
            "SELECT trustIndex from TrustLines where issuer=:v1 and balance>0 limit 1",
            use(b58Account);
        if(ledgerMaster.getDatabase().getSession().got_data())
        {
            mResultCode = txCREDIT_ISSUED;
            return false;
        }

        ledgerMaster.getDatabase().getSession() <<
            "SELECT trustIndex from TrustLines where accountID=:v1 and balance>0 limit 1",
            use(b58Account);
        if(ledgerMaster.getDatabase().getSession().got_data())
        {
            mResultCode = txCREDIT_ISSUED;
            return false;
        }
        
        std::vector<OfferFrame> retOffers;
        ledgerMaster.getDatabase().loadOffers(mSigningAccount->getID(), retOffers);
        for(auto offer : retOffers)
        {
            delta.setStart(offer);
        }

        std::vector<TrustFrame> retLines;
        ledgerMaster.getDatabase().loadLines(mSigningAccount->getID(), retLines);
        for(auto line : retLines)
        {
            delta.setStart(line);
        }

        delta.setStart(otherAccount);
        otherAccount.mEntry.account().balance += mSigningAccount->mEntry.account().balance;
        delta.setFinal(otherAccount);
        delta.removeFinal(*mSigningAccount);

        mResultCode = txSUCCESS;
        return true;
    }

    bool MergeFrame::doCheckValid(Application& app)
    {
        // makes sure not merging into self
        if(mEnvelope.tx.account == mEnvelope.tx.body.destination()) return false;
        return true;
    }
}
