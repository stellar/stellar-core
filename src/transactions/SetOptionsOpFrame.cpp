// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/SetOptionsOpFrame.h"
#include "crypto/SignerKey.h"
#include "database/Database.h"
#include "main/Application.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"

namespace stellar
{
using xdr::operator==;

static const uint32 allAccountFlags =
    (AUTH_REQUIRED_FLAG | AUTH_REVOCABLE_FLAG | AUTH_IMMUTABLE_FLAG);
static const uint32 allAccountAuthFlags =
    (AUTH_REQUIRED_FLAG | AUTH_REVOCABLE_FLAG | AUTH_IMMUTABLE_FLAG);

namespace
{

OperationResult
makeResult(SetOptionsResultCode code)
{
    auto result = OperationResult{};
    result.code(opINNER);
    result.tr().type(SET_OPTIONS);
    result.tr().setOptionsResult().code(code);
    return result;
}
}

SetOptionsOpFrame::SetOptionsOpFrame(Operation const& op,
                                     TransactionFrame& parentTx)
    : OperationFrame(op, parentTx), mSetOptions(mOperation.body.setOptionsOp())
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

OperationResult
SetOptionsOpFrame::doApply(Application& app, LedgerDelta& delta,
                           LedgerManager& ledgerManager)
{
    Database& db = ledgerManager.getDatabase();
    AccountEntry& account = mSourceAccount->getAccount();

    if (mSetOptions.inflationDest)
    {
        AccountFrame::pointer inflationAccount;
        AccountID inflationID = *mSetOptions.inflationDest;
        inflationAccount = AccountFrame::loadAccount(delta, inflationID, db);
        if (!inflationAccount)
        {
            app.getMetrics()
                .NewMeter({"op-set-options", "failure", "invalid-inflation"},
                          "operation")
                .Mark();
            return makeResult(SET_OPTIONS_INVALID_INFLATION);
        }
        account.inflationDest.activate() = inflationID;
    }

    if (mSetOptions.clearFlags)
    {
        if ((*mSetOptions.clearFlags & allAccountAuthFlags) &&
            mSourceAccount->isImmutableAuth())
        {
            app.getMetrics()
                .NewMeter({"op-set-options", "failure", "cant-change"},
                          "operation")
                .Mark();
            return makeResult(SET_OPTIONS_CANT_CHANGE);
        }
        account.flags = account.flags & ~*mSetOptions.clearFlags;
    }

    if (mSetOptions.setFlags)
    {
        if ((*mSetOptions.setFlags & allAccountAuthFlags) &&
            mSourceAccount->isImmutableAuth())
        {
            app.getMetrics()
                .NewMeter({"op-set-options", "failure", "cant-change"},
                          "operation")
                .Mark();
            return makeResult(SET_OPTIONS_CANT_CHANGE);
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
                    app.getMetrics()
                        .NewMeter(
                            {"op-set-options", "failure", "too-many-signers"},
                            "operation")
                        .Mark();
                    return makeResult(SET_OPTIONS_TOO_MANY_SIGNERS);
                }
                if (!mSourceAccount->addNumEntries(1, ledgerManager))
                {
                    app.getMetrics()
                        .NewMeter({"op-set-options", "failure", "low-reserve"},
                                  "operation")
                        .Mark();
                    return makeResult(SET_OPTIONS_LOW_RESERVE);
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

    app.getMetrics()
        .NewMeter({"op-set-options", "success", "apply"}, "operation")
        .Mark();
    mSourceAccount->storeChange(delta, db);
    return makeResult(SET_OPTIONS_SUCCESS);
}

OperationResult
SetOptionsOpFrame::doCheckValid(Application& app)
{
    if (mSetOptions.setFlags)
    {
        if (*mSetOptions.setFlags & ~allAccountFlags)
        {
            return makeResult(SET_OPTIONS_UNKNOWN_FLAG);
        }
    }

    if (mSetOptions.clearFlags)
    {
        if (*mSetOptions.clearFlags & ~allAccountFlags)
        {
            return makeResult(SET_OPTIONS_UNKNOWN_FLAG);
        }
    }

    if (mSetOptions.setFlags && mSetOptions.clearFlags)
    {
        if ((*mSetOptions.setFlags & *mSetOptions.clearFlags) != 0)
        {
            app.getMetrics()
                .NewMeter({"op-set-options", "invalid", "bad-flags"},
                          "operation")
                .Mark();
            return makeResult(SET_OPTIONS_BAD_FLAGS);
        }
    }

    if (mSetOptions.masterWeight)
    {
        if (*mSetOptions.masterWeight > UINT8_MAX)
        {
            app.getMetrics()
                .NewMeter(
                    {"op-set-options", "invalid", "threshold-out-of-range"},
                    "operation")
                .Mark();
            return makeResult(SET_OPTIONS_THRESHOLD_OUT_OF_RANGE);
        }
    }

    if (mSetOptions.lowThreshold)
    {
        if (*mSetOptions.lowThreshold > UINT8_MAX)
        {
            app.getMetrics()
                .NewMeter(
                    {"op-set-options", "invalid", "threshold-out-of-range"},
                    "operation")
                .Mark();
            return makeResult(SET_OPTIONS_THRESHOLD_OUT_OF_RANGE);
        }
    }

    if (mSetOptions.medThreshold)
    {
        if (*mSetOptions.medThreshold > UINT8_MAX)
        {
            app.getMetrics()
                .NewMeter(
                    {"op-set-options", "invalid", "threshold-out-of-range"},
                    "operation")
                .Mark();
            return makeResult(SET_OPTIONS_THRESHOLD_OUT_OF_RANGE);
        }
    }

    if (mSetOptions.highThreshold)
    {
        if (*mSetOptions.highThreshold > UINT8_MAX)
        {
            app.getMetrics()
                .NewMeter(
                    {"op-set-options", "invalid", "threshold-out-of-range"},
                    "operation")
                .Mark();
            return makeResult(SET_OPTIONS_THRESHOLD_OUT_OF_RANGE);
        }
    }

    if (mSetOptions.signer)
    {
        auto isSelf = mSetOptions.signer->key ==
                      KeyUtils::convertKey<SignerKey>(getSourceID());
        auto isPublicKey =
            KeyUtils::canConvert<PublicKey>(mSetOptions.signer->key);
        if (isSelf || (!isPublicKey &&
                       app.getLedgerManager().getCurrentLedgerVersion() < 3))
        {
            app.getMetrics()
                .NewMeter({"op-set-options", "invalid", "bad-signer"},
                          "operation")
                .Mark();
            return makeResult(SET_OPTIONS_BAD_SIGNER);
        }
    }

    if (mSetOptions.homeDomain)
    {
        if (!isString32Valid(*mSetOptions.homeDomain))
        {
            app.getMetrics()
                .NewMeter({"op-set-options", "invalid", "invalid-home-domain"},
                          "operation")
                .Mark();
            return makeResult(SET_OPTIONS_INVALID_HOME_DOMAIN);
        }
    }

    return makeResult(SET_OPTIONS_SUCCESS);
}
}
