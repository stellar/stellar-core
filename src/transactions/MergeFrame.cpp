#include "transactions/MergeFrame.h"
#include "crypto/Base58.h"
#include "database/Database.h"
#include "ledger/TrustFrame.h"

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
        Database &db = ledgerMaster.getDatabase();

        if(!AccountFrame::loadAccount(mEnvelope.tx.body.destination(),otherAccount, db))
        {
            innerResult().code(AccountMerge::NO_ACCOUNT);
            return false;
        }


        // TODO: remove direct SQL statements, use *Frame objects instead

        std::string b58Account = toBase58Check(VER_ACCOUNT_ID, mSigningAccount->getID());
        {
            auto timer = db.getSelectTimer("trust");
            db.getSession() <<
                "SELECT trustIndex from TrustLines where issuer=:v1 and balance>0 limit 1",
                use(b58Account);
        }
        if(db.getSession().got_data())
        {
            innerResult().code(AccountMerge::HAS_CREDIT);
            return false;
        }

        {
            auto timer = db.getSelectTimer("trust");
            db.getSession() <<
                "SELECT trustIndex from TrustLines where accountID=:v1 and balance>0 limit 1",
                use(b58Account);
        }
        if(db.getSession().got_data())
        {
            innerResult().code(AccountMerge::HAS_CREDIT);
            return false;
        }
        
        // delete offers
        std::vector<OfferFrame> retOffers;
        OfferFrame::loadOffers(mSigningAccount->getID(), retOffers, db);
        for(auto offer : retOffers)
        {
            offer.storeDelete(delta, db);
        }

        // delete trust lines
        std::vector<TrustFrame> retLines;
        TrustFrame::loadLines(mSigningAccount->getID(), retLines, db);
        for(auto line : retLines)
        {
            line.storeDelete(delta, db);
        }

        otherAccount.getAccount().balance += mSigningAccount->getAccount().balance;
        otherAccount.storeChange(delta, db);
        mSigningAccount->storeDelete(delta, db);

        innerResult().code(AccountMerge::SUCCESS);
        return true;
    }

    bool MergeFrame::doCheckValid(Application& app)
    {
        // makes sure not merging into self
        if (mEnvelope.tx.account == mEnvelope.tx.body.destination())
        {
            innerResult().code(AccountMerge::MALFORMED);
            return false;
        }
        return true;
    }
}
