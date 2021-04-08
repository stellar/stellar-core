// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/SetTrustLineFlagsOpFrame.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "main/Application.h"
#include "transactions/TransactionUtils.h"
#include <Tracy.hpp>

namespace stellar
{

SetTrustLineFlagsOpFrame::SetTrustLineFlagsOpFrame(Operation const& op,
                                                   OperationResult& res,
                                                   TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mSetTrustLineFlags(mOperation.body.setTrustLineFlagsOp())
{
}

ThresholdLevel
SetTrustLineFlagsOpFrame::getThresholdLevel() const
{
    return ThresholdLevel::LOW;
}

bool
SetTrustLineFlagsOpFrame::isVersionSupported(uint32_t protocolVersion) const
{
    return protocolVersion >= 17;
}

bool
SetTrustLineFlagsOpFrame::doApply(AbstractLedgerTxn& ltx)
{
    ZoneNamedN(applyZone, "SetTrustLineFlagsOp apply", true);

    if (!isAuthRevocationValid(ltx))
    {
        innerResult().code(SET_TRUST_LINE_FLAGS_CANT_REVOKE);
        return false;
    }

    auto key =
        trustlineKey(mSetTrustLineFlags.trustor, mSetTrustLineFlags.asset);
    bool shouldRemoveOffers = false;
    uint32_t expectedFlagValue = 0;
    {
        // trustline is loaded in this inner scope because it can be loaded
        // again in removeOffersByAccountAndAsset
        auto const trust = ltx.load(key);
        if (!trust)
        {
            innerResult().code(SET_TRUST_LINE_FLAGS_NO_TRUST_LINE);
            return false;
        }

        expectedFlagValue = trust.current().data.trustLine().flags;

        expectedFlagValue &= ~mSetTrustLineFlags.clearFlags;
        expectedFlagValue |= mSetTrustLineFlags.setFlags;

        if (!trustLineFlagAuthIsValid(expectedFlagValue))
        {
            innerResult().code(SET_TRUST_LINE_FLAGS_INVALID_STATE);
            return false;
        }

        shouldRemoveOffers =
            isAuthorizedToMaintainLiabilities(trust) &&
            !isAuthorizedToMaintainLiabilities(expectedFlagValue);
    }

    if (shouldRemoveOffers)
    {
        removeOffersByAccountAndAsset(ltx, mSetTrustLineFlags.trustor,
                                      mSetTrustLineFlags.asset);
    }

    auto trust = ltx.load(key);
    trust.current().data.trustLine().flags = expectedFlagValue;
    innerResult().code(SET_TRUST_LINE_FLAGS_SUCCESS);
    return true;
}

bool
SetTrustLineFlagsOpFrame::doCheckValid(uint32_t ledgerVersion)
{
    if (mSetTrustLineFlags.asset.type() == ASSET_TYPE_NATIVE)
    {
        innerResult().code(SET_TRUST_LINE_FLAGS_MALFORMED);
        return false;
    }

    if (!isAssetValid(mSetTrustLineFlags.asset))
    {
        innerResult().code(SET_TRUST_LINE_FLAGS_MALFORMED);
        return false;
    }

    if (!(getSourceID() == getIssuer(mSetTrustLineFlags.asset)))
    {
        innerResult().code(SET_TRUST_LINE_FLAGS_MALFORMED);
        return false;
    }

    if (mSetTrustLineFlags.trustor == getSourceID())
    {
        innerResult().code(SET_TRUST_LINE_FLAGS_MALFORMED);
        return false;
    }

    if ((mSetTrustLineFlags.setFlags & mSetTrustLineFlags.clearFlags) != 0)
    {
        innerResult().code(SET_TRUST_LINE_FLAGS_MALFORMED);
        return false;
    }

    // setFlags has the same restrictions as the trustline flags, so we can
    // use trustLineFlagIsValid here
    if (!trustLineFlagIsValid(mSetTrustLineFlags.setFlags, ledgerVersion) ||
        (mSetTrustLineFlags.setFlags & TRUSTLINE_CLAWBACK_ENABLED_FLAG) != 0)
    {
        innerResult().code(SET_TRUST_LINE_FLAGS_MALFORMED);
        return false;
    }

    if (!trustLineFlagMaskCheckIsValid(mSetTrustLineFlags.clearFlags,
                                       ledgerVersion))
    {
        innerResult().code(SET_TRUST_LINE_FLAGS_MALFORMED);
        return false;
    }

    return true;
}

void
SetTrustLineFlagsOpFrame::insertLedgerKeysToPrefetch(
    UnorderedSet<LedgerKey>& keys) const
{
    if (mSetTrustLineFlags.asset.type() == ASSET_TYPE_NATIVE)
    {
        throw std::runtime_error("cannot prefetch native asset");
    }

    keys.emplace(
        trustlineKey(mSetTrustLineFlags.trustor, mSetTrustLineFlags.asset));
}

bool
SetTrustLineFlagsOpFrame::isAuthRevocationValid(AbstractLedgerTxn& ltx)
{
    LedgerTxn ltxSource(ltx); // ltxSource will be rolled back
    auto header = ltxSource.loadHeader();
    auto sourceAccountEntry = loadSourceAccount(ltxSource, header);

    if ((sourceAccountEntry.current().data.account().flags &
         AUTH_REVOCABLE_FLAG) != 0)
    {
        return true;
    }

    // AUTH_REVOCABLE_FLAG is not set on the account, so the three transitions
    // below are not allowed.
    //
    // 1. AUTHORIZED_FLAG -> AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG
    // 2. AUTHORIZED_FLAG -> 0
    // 3. AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG -> 0
    //
    // The condition below will return false if one of the above transitions is
    // made. In all three scenarios above, clearingAnyAuth == true and
    // settingAuthorized == false

    bool clearingAnyAuth =
        (mSetTrustLineFlags.clearFlags & TRUSTLINE_AUTH_FLAGS) != 0;

    bool settingAuthorized =
        (mSetTrustLineFlags.setFlags & AUTHORIZED_FLAG) != 0;

    return !clearingAnyAuth || settingAuthorized;
}
}