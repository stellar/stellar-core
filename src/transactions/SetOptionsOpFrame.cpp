// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "transactions/SetOptionsOpFrame.h"
#include "crypto/Base58.h"
#include "database/Database.h"

// TODO.2 Handle all SQL exceptions
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
    return mSourceAccount->getMidThreshold();
}

// make sure it doesn't allow us to add signers when we don't have the
// minbalance
bool
SetOptionsOpFrame::doApply(LedgerDelta& delta, LedgerManagerImpl& ledgerMaster)
{
    Database& db = ledgerMaster.getDatabase();
    AccountEntry& account = mSourceAccount->getAccount();

    if (mSetOptions.inflationDest)
    {
        account.inflationDest.activate() = *mSetOptions.inflationDest;
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
                    innerResult().code(SetOptions::MALFORMED);
                    return false;
                }
                if (account.balance <
                    ledgerMaster.getMinBalance(account.numSubEntries + 1))
                {
                    innerResult().code(SetOptions::BELOW_MIN_BALANCE);
                    return false;
                }
                account.numSubEntries++;
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
                    account.numSubEntries--;
                }
                else
                {
                    it++;
                }
            }
        }
        mSourceAccount->setUpdateSigners();
    }

    innerResult().code(SetOptions::SUCCESS);
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
            innerResult().code(SetOptions::MALFORMED);
            return false;
        }
    }
    return true;
}
}
