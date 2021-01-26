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
#include "transactions/SponsorshipUtils.h"
#include "transactions/TransactionUtils.h"
#include "util/XDROperators.h"
#include <Tracy.hpp>

namespace stellar
{

static const uint32 allAccountAuthFlags =
    (AUTH_REQUIRED_FLAG | AUTH_REVOCABLE_FLAG | AUTH_IMMUTABLE_FLAG |
     AUTH_CLAWBACK_ENABLED_FLAG);

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
SetOptionsOpFrame::addOrChangeSigner(AbstractLedgerTxn& ltx)
{
    auto header = ltx.loadHeader();
    auto sourceAccount = loadSourceAccount(ltx, header);

    auto& account = sourceAccount.current().data.account();
    auto& signers = account.signers;

    // Change signer
    auto findRes = findSignerByKey(signers.begin(), signers.end(),
                                   mSetOptions.signer->key);
    if (findRes.second)
    {
        findRes.first->weight = mSetOptions.signer->weight;
        return true;
    }

    // Add signer
    if (signers.size() == signers.max_size())
    {
        innerResult().code(SET_OPTIONS_TOO_MANY_SIGNERS);
        return false;
    }

    auto it = signers.insert(findRes.first, *mSetOptions.signer);

    if (hasAccountEntryExtV2(account))
    {
        size_t n = it - account.signers.begin();
        auto& extV2 = account.ext.v1().ext.v2();
        // There must always be an element in signerSponsoringIDs corresponding
        // to each element in signers. Because the signer is not sponsored, the
        // relevant signerSponsoringID must be null.
        extV2.signerSponsoringIDs.insert(extV2.signerSponsoringIDs.begin() + n,
                                         SponsorshipDescriptor{});
    }

    switch (createSignerWithPossibleSponsorship(ltx, header, it, sourceAccount))
    {
    case SponsorshipResult::SUCCESS:
        break;
    case SponsorshipResult::LOW_RESERVE:
        innerResult().code(SET_OPTIONS_LOW_RESERVE);
        return false;
    case SponsorshipResult::TOO_MANY_SUBENTRIES:
        mResult.code(opTOO_MANY_SUBENTRIES);
        return false;
    case SponsorshipResult::TOO_MANY_SPONSORING:
        mResult.code(opTOO_MANY_SPONSORING);
        return false;
    case SponsorshipResult::TOO_MANY_SPONSORED:
        // This is impossible right now because there is a limit on sub
        // entries, fall through and throw
    default:
        throw std::runtime_error(
            "Unexpected result from createSignerWithPossibleSponsorship");
    }
    return true;
}

void
SetOptionsOpFrame::deleteSigner(AbstractLedgerTxn& ltx,
                                LedgerTxnHeader const& header,
                                LedgerTxnEntry& sourceAccount)
{
    auto& account = sourceAccount.current().data.account();
    auto& signers = account.signers;

    auto findRes = findSignerByKey(signers.begin(), signers.end(),
                                   mSetOptions.signer->key);
    if (findRes.second)
    {
        removeSignerWithPossibleSponsorship(ltx, header, findRes.first,
                                            sourceAccount);
    }
}

bool
SetOptionsOpFrame::doApply(AbstractLedgerTxn& ltx)
{
    ZoneNamedN(applyZone, "SetOptionsOp apply", true);

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

    // ensure that revocable is set if clawback is set
    if (mSetOptions.setFlags || mSetOptions.clearFlags)
    {
        if (!accountFlagClawbackIsValid(account.flags,
                                        header.current().ledgerVersion))
        {
            innerResult().code(SET_OPTIONS_AUTH_REVOCABLE_REQUIRED);
            return false;
        }
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
        if (mSetOptions.signer->weight)
        {
            LedgerTxn ltxInner(ltx);
            if (!addOrChangeSigner(ltxInner))
            {
                return false;
            }
            ltxInner.commit();
        }
        else
        {
            deleteSigner(ltx, header, sourceAccount);
        }
    }

    innerResult().code(SET_OPTIONS_SUCCESS);
    return true;
}

bool
SetOptionsOpFrame::doCheckValid(uint32_t ledgerVersion)
{
    if ((mSetOptions.setFlags &&
         !accountFlagMaskCheckIsValid(*mSetOptions.setFlags, ledgerVersion)) ||
        (mSetOptions.clearFlags &&
         !accountFlagMaskCheckIsValid(*mSetOptions.clearFlags, ledgerVersion)))
    {
        innerResult().code(SET_OPTIONS_UNKNOWN_FLAG);
        return false;
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
