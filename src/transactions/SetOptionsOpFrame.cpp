// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/SetOptionsOpFrame.h"
#include "crypto/Base58.h"
#include "database/Database.h"

namespace stellar
{
SetOptionsOpFrame::SetOptionsOpFrame(Operation const& op, OperationResult& res,
                                     TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mSetOptions(mOperation.body.setOptionsOp())
{
}

int32_t
SetOptionsOpFrame::getNeededThreshold() const
{
    // updating thresholds or signer requires high threshold
    if (mSetOptions.thresholds || mSetOptions.signer)
    {
        return mSourceAccount->getHighThreshold();
    }
    return mSourceAccount->getMediumThreshold();
}

bool
SetOptionsOpFrame::doApply(LedgerDelta& delta, LedgerManager& ledgerManager)
{
    Database& db = ledgerManager.getDatabase();
    AccountEntry& account = mSourceAccount->getAccount();

    if (mSetOptions.inflationDest)
    {
        AccountFrame inflationAccount;
        AccountID inflationID = *mSetOptions.inflationDest;
        if (!AccountFrame::loadAccount(inflationID, inflationAccount, db))
        {
            innerResult().code(SET_OPTIONS_INVALID_INFLATION);
            return false;
        }
        account.inflationDest.activate() = inflationID;
    }

    if (mSetOptions.clearFlags)
    {
        account.flags = account.flags & ~*mSetOptions.clearFlags;
    }
    if (mSetOptions.setFlags)
    {
        account.flags = account.flags | *mSetOptions.setFlags;
    }

    if (mSetOptions.thresholds)
    {
        account.thresholds = *mSetOptions.thresholds;
    }

    if (mSetOptions.signer)
    {
        auto& signers = account.signers;
        if (mSetOptions.signer->weight)
        { // add or change signer
            bool found = false;
            for (auto oldSigner : signers)
            {
                if (oldSigner.pubKey == mSetOptions.signer->pubKey)
                {
                    oldSigner.weight = mSetOptions.signer->weight;
                }
            }
            if (!found)
            {
                if (signers.size() == signers.max_size())
                {
                    innerResult().code(SET_OPTIONS_TOO_MANY_SIGNERS);
                    return false;
                }
                if (!mSourceAccount->addNumEntries(1, ledgerManager))
                {
                    innerResult().code(SET_OPTIONS_LOW_RESERVE);
                    return false;
                }
                signers.push_back(*mSetOptions.signer);
            }
        }
        else
        { // delete signer
            auto it = signers.begin();
            while (it != signers.end())
            {
                Signer& oldSigner = *it;
                if (oldSigner.pubKey == mSetOptions.signer->pubKey)
                {
                    it = signers.erase(it);
                    mSourceAccount->addNumEntries(-1, ledgerManager);
                }
                else
                {
                    it++;
                }
            }
        }
        mSourceAccount->setUpdateSigners();
    }

    innerResult().code(SET_OPTIONS_SUCCESS);
    mSourceAccount->storeChange(delta, db);
    return true;
}

bool
SetOptionsOpFrame::doCheckValid(Application& app)
{
    if (mSetOptions.setFlags && mSetOptions.clearFlags)
    {
        if ((*mSetOptions.setFlags & *mSetOptions.clearFlags) != 0)
        {
            innerResult().code(SET_OPTIONS_BAD_FLAGS);
            return false;
        }
    }
    return true;
}
}
