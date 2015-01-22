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
    bool MergeFrame::doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster)
    {
        AccountFrame otherAccount;
        if(!ledgerMaster.getDatabase().loadAccount(mEnvelope.tx.body.destination(),otherAccount))
        {
            innerResult().result.code(AccountMerge::NO_ACCOUNT);
            return false;
        }

        std::string b58Account = toBase58Check(VER_ACCOUNT_ID, mSigningAccount->getID());
        ledgerMaster.getDatabase().getSession() <<
            "SELECT trustIndex from TrustLines where issuer=:v1 and balance>0 limit 1",
            use(b58Account);
        if(ledgerMaster.getDatabase().getSession().got_data())
        {
            innerResult().result.code(AccountMerge::HAS_CREDIT);
            return false;
        }

        ledgerMaster.getDatabase().getSession() <<
            "SELECT trustIndex from TrustLines where accountID=:v1 and balance>0 limit 1",
            use(b58Account);
        if(ledgerMaster.getDatabase().getSession().got_data())
        {
            innerResult().result.code(AccountMerge::HAS_CREDIT);
            return false;
        }
        
        // delete offers
        std::vector<OfferFrame> retOffers;
        ledgerMaster.getDatabase().loadOffers(mSigningAccount->getID(), retOffers);
        for(auto offer : retOffers)
        {
            offer.storeDelete(delta, ledgerMaster);
        }

        // delete trust lines
        std::vector<TrustFrame> retLines;
        ledgerMaster.getDatabase().loadLines(mSigningAccount->getID(), retLines);
        for(auto line : retLines)
        {
            line.storeDelete(delta, ledgerMaster);
        }

        otherAccount.mEntry.account().balance += mSigningAccount->mEntry.account().balance;
        otherAccount.storeChange(delta, ledgerMaster);
        mSigningAccount->storeDelete(delta, ledgerMaster);

        innerResult().result.code(AccountMerge::SUCCESS);
        return true;
    }

    bool MergeFrame::doCheckValid(Application& app)
    {
        // makes sure not merging into self
        if (mEnvelope.tx.account == mEnvelope.tx.body.destination())
        {
            innerResult().result.code(AccountMerge::MALFORMED);
            return false;
        }
        return true;
    }
}
