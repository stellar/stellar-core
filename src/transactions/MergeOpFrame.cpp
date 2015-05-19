// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

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
    AccountFrame::pointer otherAccount;
    Database& db = ledgerManager.getDatabase();

    otherAccount = AccountFrame::loadAccount(mOperation.body.destination(), db);

    if (!otherAccount)
    {
        innerResult().code(ACCOUNT_MERGE_NO_ACCOUNT);
        return false;
    }

    if (TrustFrame::hasIssued(getSourceID(), db))
    {
        innerResult().code(ACCOUNT_MERGE_CREDIT_HELD);
        return false;
    }

    std::vector<TrustFrame::pointer> lines;
    TrustFrame::loadLines(getSourceID(), lines, db);
    for(auto &l : lines)
    {
        if(l->getBalance() > 0)
        {
            innerResult().code(ACCOUNT_MERGE_HAS_CREDIT);
            return false;
        }
    }

    // delete offers
    std::vector<OfferFrame::pointer> offers;
    OfferFrame::loadOffers(getSourceID(), offers, db);
    for (auto& offer : offers)
    {
        offer->storeDelete(delta, db);
    }

    // delete trust lines
    for (auto& l : lines)
    {
        l->storeDelete(delta, db);
    }

    otherAccount->getAccount().balance += mSourceAccount->getAccount().balance;
    otherAccount->storeChange(delta, db);
    mSourceAccount->storeDelete(delta, db);

    innerResult().code(ACCOUNT_MERGE_SUCCESS);
    return true;
}

bool
MergeOpFrame::doCheckValid()
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
