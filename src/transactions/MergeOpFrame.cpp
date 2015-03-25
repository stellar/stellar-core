// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "transactions/MergeOpFrame.h"
#include "crypto/Base58.h"
#include "database/Database.h"
#include "ledger/TrustFrame.h"

using namespace soci;

namespace stellar
{
MergeOpFrame::MergeOpFrame(Operation const& op, OperationResult& res,
                           TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
{
}

int32_t
MergeOpFrame::getNeededThreshold() const
{
    return mSourceAccount->getHighThreshold();
}

// make sure the deleted Account hasn't issued credit
// make sure we aren't holding any credit
// make sure the we delete all the offers
// make sure the we delete all the trustlines
// move the XLM to the new account
bool
MergeOpFrame::doApply(LedgerDelta& delta, LedgerManager& ledgerManager)
{
    AccountFrame otherAccount;
    Database& db = ledgerManager.getDatabase();

    if (!AccountFrame::loadAccount(mOperation.body.destination(), otherAccount,
                                   db))
    {
        innerResult().code(ACCOUNT_MERGE_NO_ACCOUNT);
        return false;
    }

    // TODO: remove direct SQL statements, use *Frame objects instead

    std::string b58Account = toBase58Check(VER_ACCOUNT_ID, getSourceID());
    {
        auto timer = db.getSelectTimer("trust");
        db.getSession() << "SELECT trustIndex from TrustLines where issuer=:v1 "
                           "and balance>0 limit 1",
            use(b58Account);
    }
    if (db.getSession().got_data())
    {
        innerResult().code(ACCOUNT_MERGE_CREDIT_HELD);
        return false;
    }

    {
        auto timer = db.getSelectTimer("trust");
        db.getSession() << "SELECT trustIndex from TrustLines where "
                           "accountID=:v1 and balance>0 limit 1",
            use(b58Account);
    }
    if (db.getSession().got_data())
    {
        innerResult().code(ACCOUNT_MERGE_HAS_CREDIT);
        return false;
    }

    // delete offers
    std::vector<OfferFrame> retOffers;
    OfferFrame::loadOffers(getSourceID(), retOffers, db);
    for (auto offer : retOffers)
    {
        offer.storeDelete(delta, db);
    }

    // delete trust lines
    std::vector<TrustFrame> retLines;
    TrustFrame::loadLines(getSourceID(), retLines, db);
    for (auto line : retLines)
    {
        line.storeDelete(delta, db);
    }

    otherAccount.getAccount().balance += mSourceAccount->getAccount().balance;
    otherAccount.storeChange(delta, db);
    mSourceAccount->storeDelete(delta, db);

    innerResult().code(ACCOUNT_MERGE_SUCCESS);
    return true;
}

bool
MergeOpFrame::doCheckValid(Application& app)
{
    // makes sure not merging into self
    if (getSourceID() == mOperation.body.destination())
    {
        innerResult().code(ACCOUNT_MERGE_MALFORMED);
        return false;
    }
    return true;
}
}
