// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/TrustFlagsOpFrameBase.h"
#include "ledger/LedgerTxn.h"
#include "transactions/TransactionUtils.h"
#include "util/ProtocolVersion.h"
#include <Tracy.hpp>

namespace stellar
{

TrustFlagsOpFrameBase::TrustFlagsOpFrameBase(Operation const& op,
                                             TransactionFrame const& parentTx)
    : OperationFrame(op, parentTx)
{
}

ThresholdLevel
TrustFlagsOpFrameBase::getThresholdLevel() const
{
    return ThresholdLevel::LOW;
}

bool
TrustFlagsOpFrameBase::removeOffers(AbstractLedgerTxn& ltx,
                                    OperationResult& res) const
{
    // Delete all offers owned by the trustor that are either buying or
    // selling the asset which had authorization revoked. Also redeem pool
    // share trustlines owned by the trustor that use this asset
    auto removeResult = removeOffersAndPoolShareTrustLines(
        ltx, getOpTrustor(), getOpAsset(), mParentTx.getSourceID(),
        mParentTx.getSeqNum(), getOpIndex());

    switch (removeResult)
    {
    case RemoveResult::SUCCESS:
        break;
    case RemoveResult::LOW_RESERVE:
        setResultLowReserve(res);
        return false;
    case RemoveResult::TOO_MANY_SPONSORING:
        res.code(opTOO_MANY_SPONSORING);
        return false;
    default:
        throw std::runtime_error("Unexpected RemoveResult");
    }
    return true;
}

bool
TrustFlagsOpFrameBase::doApply(AppConnector& app, AbstractLedgerTxn& ltx,
                               Hash const& sorobanBasePrngSeed,
                               OperationResult& res,
                               std::shared_ptr<SorobanTxData> sorobanData) const
{
    ZoneNamedN(applyZone, "TrustFlagsOpFrameBase apply", true);

    auto ledgerVersion = ltx.loadHeader().current().ledgerVersion;
    if (protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_3))
    {
        // Only relevant for AllowTrust, since version 3 it is not allowed
        // to use AllowTrust on self.
        // In SetTrustLineFlags (exists since version 17), trust-to-self is
        // explicitly disallowed in doCheckValid.
        if (getOpTrustor() == getSourceID())
        {
            setResultSelfNotAllowed(res);
            return false;
        }
    }

    bool authRevocable = true;
    if (!isAuthRevocationValid(ltx, authRevocable, res))
    {
        return false;
    }

    if (getOpTrustor() == getSourceID())
    {
        // Only relevant for AllowTrust, possible for version <= 2.
        // In SetTrustLineFlags, trust-to-self is explicitly disallowed
        // in doCheckValid.
        setResultSuccess(res);
        return true;
    }

    auto key = trustlineKey(getOpTrustor(), getOpAsset());
    bool shouldRemoveOffers = false;
    uint32_t expectedFlagValue = 0;
    {
        // trustline is loaded in this inner scope because it can be loaded
        // again in removeOffers
        auto const trust = ltx.load(key);
        if (!trust)
        {
            setResultNoTrustLine(res);
            return false;
        }

        // Calc expected flag value of this trustline.
        if (!calcExpectedFlagValue(trust, expectedFlagValue, res))
        {
            return false;
        }

        // Check the auth revoc valid for the 2nd time, only needed for
        // AllowTrust
        if (!isRevocationToMaintainLiabilitiesValid(authRevocable, trust,
                                                    expectedFlagValue, res))
        {
            return false;
        }

        // Check if need to remove offer
        shouldRemoveOffers =
            isAuthorizedToMaintainLiabilities(trust) &&
            !isAuthorizedToMaintainLiabilitiesUnsafe(expectedFlagValue);
    }

    // Remove offers, the ledgerVersion check is only relevant for AllowTrust
    if (protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_10) &&
        shouldRemoveOffers)
    {
        if (!removeOffers(ltx, res))
        {
            return false;
        }
    }

    // Set value
    setFlagValue(ltx, key, expectedFlagValue);
    setResultSuccess(res);
    return true;
}

}
