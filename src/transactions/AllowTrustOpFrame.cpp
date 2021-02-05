// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/AllowTrustOpFrame.h"
#include "database/Database.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/TrustLineWrapper.h"
#include "main/Application.h"
#include "transactions/SponsorshipUtils.h"
#include "transactions/TransactionUtils.h"
#include <Tracy.hpp>

namespace stellar
{

void
setAuthorized(LedgerTxnHeader const& header, LedgerTxnEntry& entry,
              uint32_t authorized)
{
    if (!trustLineFlagIsValid(authorized, header))
    {
        throw std::runtime_error("trying to set invalid trust line flag");
    }

    const uint32_t authFlags =
        AUTHORIZED_FLAG | AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG;

    if ((authorized & ~authFlags) != 0)
    {
        throw std::runtime_error(
            "setAuthorized can only modify authorization flags");
    }

    auto& tl = entry.current().data.trustLine();

    tl.flags &= ~authFlags;
    tl.flags |= authorized;
}

AllowTrustOpFrame::AllowTrustOpFrame(Operation const& op, OperationResult& res,
                                     TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mAllowTrust(mOperation.body.allowTrustOp())
    , mAsset(getAsset(getSourceID(), mAllowTrust.asset))
{
}

ThresholdLevel
AllowTrustOpFrame::getThresholdLevel() const
{
    return ThresholdLevel::LOW;
}

bool
AllowTrustOpFrame::doApply(AbstractLedgerTxn& ltx)
{
    ZoneNamedN(applyZone, "AllowTrustOp apply", true);
    if (ltx.loadHeader().current().ledgerVersion > 2)
    {
        if (mAllowTrust.trustor == getSourceID())
        {
            // since version 3 it is not allowed to use ALLOW_TRUST on self
            innerResult().code(ALLOW_TRUST_SELF_NOT_ALLOWED);
            return false;
        }
    }

    bool authNotRevocable;
    {
        LedgerTxn ltxSource(ltx); // ltxSource will be rolled back
        auto header = ltxSource.loadHeader();
        auto sourceAccountEntry = loadSourceAccount(ltxSource, header);
        auto const& sourceAccount = sourceAccountEntry.current().data.account();
        if (!(sourceAccount.flags & AUTH_REQUIRED_FLAG))
        { // this account doesn't require authorization to
            // hold credit
            innerResult().code(ALLOW_TRUST_TRUST_NOT_REQUIRED);
            return false;
        }

        authNotRevocable = !(sourceAccount.flags & AUTH_REVOCABLE_FLAG);
        if (authNotRevocable && mAllowTrust.authorize == 0)
        {
            innerResult().code(ALLOW_TRUST_CANT_REVOKE);
            return false;
        }
    }

    // Only possible in ledger version 1 and 2
    if (mAllowTrust.trustor == getSourceID())
    {
        innerResult().code(ALLOW_TRUST_SUCCESS);
        return true;
    }

    LedgerKey key(TRUSTLINE);
    key.trustLine().accountID = mAllowTrust.trustor;
    key.trustLine().asset = mAsset;

    bool shouldRemoveOffers = false;
    {
        auto trust = ltx.load(key);
        if (!trust)
        {
            innerResult().code(ALLOW_TRUST_NO_TRUST_LINE);
            return false;
        }

        // There are two cases where we set the result to
        // ALLOW_TRUST_CANT_REVOKE -
        // 1. We try to revoke authorization when AUTH_REVOCABLE_FLAG is not set
        // (This is done above when we call loadSourceAccount)
        // 2. We try to go from AUTHORIZED_FLAG to
        // AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG when AUTH_REVOCABLE_FLAG is
        // not set
        if (authNotRevocable &&
            (isAuthorized(trust) &&
             (mAllowTrust.authorize & AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG)))
        {
            innerResult().code(ALLOW_TRUST_CANT_REVOKE);
            return false;
        }

        shouldRemoveOffers = isAuthorizedToMaintainLiabilities(trust) &&
                             mAllowTrust.authorize == 0;
    }

    auto header = ltx.loadHeader();
    if (header.current().ledgerVersion >= 10 && shouldRemoveOffers)
    {
        // Delete all offers owned by the trustor that are either buying or
        // selling the asset which had authorization revoked.
        auto offers =
            ltx.loadOffersByAccountAndAsset(mAllowTrust.trustor, mAsset);
        for (auto& offer : offers)
        {
            auto const& oe = offer.current().data.offer();
            if (!(oe.sellerID == mAllowTrust.trustor))
            {
                throw std::runtime_error("Offer not owned by expected account");
            }
            else if (!(oe.buying == mAsset || oe.selling == mAsset))
            {
                throw std::runtime_error(
                    "Offer not buying or selling expected asset");
            }

            releaseLiabilities(ltx, header, offer);
            auto trustAcc = stellar::loadAccount(ltx, mAllowTrust.trustor);
            removeEntryWithPossibleSponsorship(ltx, header, offer.current(),
                                               trustAcc);
            offer.erase();
        }
    }

    auto trustLineEntry = ltx.load(key);
    setAuthorized(header, trustLineEntry, mAllowTrust.authorize);

    innerResult().code(ALLOW_TRUST_SUCCESS);
    return true;
}

bool
AllowTrustOpFrame::doCheckValid(uint32_t ledgerVersion)
{
    if (mAllowTrust.asset.type() == ASSET_TYPE_NATIVE)
    {
        innerResult().code(ALLOW_TRUST_MALFORMED);
        return false;
    }

    if ((mAllowTrust.authorize & TRUSTLINE_CLAWBACK_ENABLED_FLAG) != 0)
    {
        innerResult().code(ALLOW_TRUST_MALFORMED);
        return false;
    }

    if (!trustLineFlagIsValid(mAllowTrust.authorize, ledgerVersion))
    {
        innerResult().code(ALLOW_TRUST_MALFORMED);
        return false;
    }

    if (!isAssetValid(mAsset))
    {
        innerResult().code(ALLOW_TRUST_MALFORMED);
        return false;
    }

    if (ledgerVersion > 15 && mAllowTrust.trustor == getSourceID())
    {
        innerResult().code(ALLOW_TRUST_MALFORMED);
        return false;
    }

    return true;
}
}
