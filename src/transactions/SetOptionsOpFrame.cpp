// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/SetOptionsOpFrame.h"
#include "crypto/SignerKey.h"
#include "database/Database.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "main/Application.h"
#include "transactions/TransactionUtils.h"
#include "util/XDROperators.h"

namespace stellar
{

static const uint32 allAccountFlags =
    (AUTH_REQUIRED_FLAG | AUTH_REVOCABLE_FLAG | AUTH_IMMUTABLE_FLAG);
static const uint32 allAccountAuthFlags =
    (AUTH_REQUIRED_FLAG | AUTH_REVOCABLE_FLAG | AUTH_IMMUTABLE_FLAG);

SetOptionsOpFrame::SetOptionsOpFrame(Operation const& op, OperationResult& res,
                                     TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mSetOptions(mOperation.body.setOptionsOp())
{
}

ThresholdLevel
SetOptionsOpFrame::getThresholdLevel() const
{
    // updating thresholds or signer requires high threshold
    if (mSetOptions.masterWeight || mSetOptions.lowThreshold ||
        mSetOptions.medThreshold || mSetOptions.highThreshold ||
        mSetOptions.signer)
    {
        return ThresholdLevel::HIGH;
    }
    return ThresholdLevel::MEDIUM;
}

bool
SetOptionsOpFrame::doApply(Application& app, AbstractLedgerTxn& ltx)
{
    auto header = ltx.loadHeader();
    auto sourceAccount = loadSourceAccount(ltx, header);
    auto& account = sourceAccount.current().data.account();
    if (mSetOptions.inflationDest)
    {
        AccountID inflationID = *mSetOptions.inflationDest;
        if (!(inflationID == getSourceID()))
        {
            if (!stellar::loadAccountWithoutRecord(ltx, inflationID))
            {
                innerResult().code(SET_OPTIONS_INVALID_INFLATION);
                return false;
            }
        }
        account.inflationDest.activate() = inflationID;
    }

    if (mSetOptions.clearFlags)
    {
        if ((*mSetOptions.clearFlags & allAccountAuthFlags) &&
            isImmutableAuth(sourceAccount))
        {
            innerResult().code(SET_OPTIONS_CANT_CHANGE);
            return false;
        }
        account.flags = account.flags & ~*mSetOptions.clearFlags;
    }

    if (mSetOptions.setFlags)
    {
        if ((*mSetOptions.setFlags & allAccountAuthFlags) &&
            isImmutableAuth(sourceAccount))
        {
            innerResult().code(SET_OPTIONS_CANT_CHANGE);
            return false;
        }
        account.flags = account.flags | *mSetOptions.setFlags;
    }

    if (mSetOptions.homeDomain)
    {
        account.homeDomain = *mSetOptions.homeDomain;
    }

    if (mSetOptions.masterWeight)
    {
        account.thresholds[THRESHOLD_MASTER_WEIGHT] =
            *mSetOptions.masterWeight & UINT8_MAX;
    }

    if (mSetOptions.lowThreshold)
    {
        account.thresholds[THRESHOLD_LOW] =
            *mSetOptions.lowThreshold & UINT8_MAX;
    }

    if (mSetOptions.medThreshold)
    {
        account.thresholds[THRESHOLD_MED] =
            *mSetOptions.medThreshold & UINT8_MAX;
    }

    if (mSetOptions.highThreshold)
    {
        account.thresholds[THRESHOLD_HIGH] =
            *mSetOptions.highThreshold & UINT8_MAX;
    }

    if (mSetOptions.signer)
    {
        auto& signers = account.signers;
        if (mSetOptions.signer->weight)
        { // add or change signer
            bool found = false;
            for (auto& oldSigner : signers)
            {
                if (oldSigner.key == mSetOptions.signer->key)
                {
                    oldSigner.weight = mSetOptions.signer->weight;
                    found = true;
                }
            }
            if (!found)
            {
                if (signers.size() == signers.max_size())
                {
                    innerResult().code(SET_OPTIONS_TOO_MANY_SIGNERS);
                    return false;
                }
                if (!addNumEntries(header, sourceAccount, 1))
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
                if (oldSigner.key == mSetOptions.signer->key)
                {
                    it = signers.erase(it);
                    addNumEntries(header, sourceAccount, -1);
                }
                else
                {
                    it++;
                }
            }
        }
        normalizeSigners(sourceAccount);
    }

    innerResult().code(SET_OPTIONS_SUCCESS);
    return true;
}

bool
SetOptionsOpFrame::doCheckValid(Application& app, uint32_t ledgerVersion)
{
    if (mSetOptions.setFlags)
    {
        if (*mSetOptions.setFlags & ~allAccountFlags)
        {
            innerResult().code(SET_OPTIONS_UNKNOWN_FLAG);
            return false;
        }
    }

    if (mSetOptions.clearFlags)
    {
        if (*mSetOptions.clearFlags & ~allAccountFlags)
        {
            innerResult().code(SET_OPTIONS_UNKNOWN_FLAG);
            return false;
        }
    }

    if (mSetOptions.setFlags && mSetOptions.clearFlags)
    {
        if ((*mSetOptions.setFlags & *mSetOptions.clearFlags) != 0)
        {
            innerResult().code(SET_OPTIONS_BAD_FLAGS);
            return false;
        }
    }

    if (mSetOptions.masterWeight)
    {
        if (*mSetOptions.masterWeight > UINT8_MAX)
        {
            innerResult().code(SET_OPTIONS_THRESHOLD_OUT_OF_RANGE);
            return false;
        }
    }

    if (mSetOptions.lowThreshold)
    {
        if (*mSetOptions.lowThreshold > UINT8_MAX)
        {
            innerResult().code(SET_OPTIONS_THRESHOLD_OUT_OF_RANGE);
            return false;
        }
    }

    if (mSetOptions.medThreshold)
    {
        if (*mSetOptions.medThreshold > UINT8_MAX)
        {
            innerResult().code(SET_OPTIONS_THRESHOLD_OUT_OF_RANGE);
            return false;
        }
    }

    if (mSetOptions.highThreshold)
    {
        if (*mSetOptions.highThreshold > UINT8_MAX)
        {
            innerResult().code(SET_OPTIONS_THRESHOLD_OUT_OF_RANGE);
            return false;
        }
    }

    if (mSetOptions.signer)
    {
        auto isSelf = mSetOptions.signer->key ==
                      KeyUtils::convertKey<SignerKey>(getSourceID());
        auto isPublicKey =
            KeyUtils::canConvert<PublicKey>(mSetOptions.signer->key);
        if (isSelf || (!isPublicKey && ledgerVersion < 3))
        {
            innerResult().code(SET_OPTIONS_BAD_SIGNER);
            return false;
        }
        if (mSetOptions.signer->weight > UINT8_MAX && ledgerVersion > 9)
        {
            innerResult().code(SET_OPTIONS_BAD_SIGNER);
            return false;
        }
    }

    if (mSetOptions.homeDomain)
    {
        if (!isString32Valid(*mSetOptions.homeDomain))
        {
            innerResult().code(SET_OPTIONS_INVALID_HOME_DOMAIN);
            return false;
        }
    }

    return true;
}
}
