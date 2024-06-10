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
#include "transactions/TransactionUtils.h"
#include "util/ProtocolVersion.h"
#include <Tracy.hpp>

namespace stellar
{

AllowTrustOpFrame::AllowTrustOpFrame(Operation const& op,
                                     TransactionFrame const& parentTx,
                                     uint32_t index)
    : TrustFlagsOpFrameBase(op, parentTx)
    , mAllowTrust(mOperation.body.allowTrustOp())
    , mAsset(getAsset(getSourceID(), mAllowTrust.asset))
    , mOpIndex(index)
{
}

void
AllowTrustOpFrame::setResultSelfNotAllowed(OperationResult& res) const
{
    innerResult(res).code(ALLOW_TRUST_SELF_NOT_ALLOWED);
}

void
AllowTrustOpFrame::setResultNoTrustLine(OperationResult& res) const
{
    innerResult(res).code(ALLOW_TRUST_NO_TRUST_LINE);
}

void
AllowTrustOpFrame::setResultLowReserve(OperationResult& res) const
{
    innerResult(res).code(ALLOW_TRUST_LOW_RESERVE);
}

void
AllowTrustOpFrame::setResultSuccess(OperationResult& res) const
{
    innerResult(res).code(ALLOW_TRUST_SUCCESS);
}

AccountID const&
AllowTrustOpFrame::getOpTrustor() const
{
    return mAllowTrust.trustor;
}

Asset const&
AllowTrustOpFrame::getOpAsset() const
{
    return mAsset;
}

uint32_t
AllowTrustOpFrame::getOpIndex() const
{
    return mOpIndex;
}

bool
AllowTrustOpFrame::calcExpectedFlagValue(LedgerTxnEntry const& trust,
                                         uint32_t& expectedVal,
                                         OperationResult& res) const
{
    expectedVal = trust.current().data.trustLine().flags;
    expectedVal &= ~TRUSTLINE_AUTH_FLAGS;
    expectedVal |= mAllowTrust.authorize;
    return true;
}

void
AllowTrustOpFrame::setFlagValue(AbstractLedgerTxn& ltx, LedgerKey const& key,
                                uint32_t flagVal) const
{
    if (!trustLineFlagIsValid(mAllowTrust.authorize, ltx.loadHeader()))
    {
        throw std::runtime_error("trying to set invalid trust line flag");
    }

    if ((mAllowTrust.authorize & ~TRUSTLINE_AUTH_FLAGS) != 0)
    {
        throw std::runtime_error(
            "AllowTrustOp can only modify authorization flags");
    }

    auto trust = ltx.load(key);
    trust.current().data.trustLine().flags = flagVal;
}

bool
AllowTrustOpFrame::isAuthRevocationValid(AbstractLedgerTxn& ltx,
                                         bool& authRevocable,
                                         OperationResult& res) const
{
    // Load the source account
    LedgerTxn ltxSource(ltx); // ltxSource will be rolled back
    auto header = ltxSource.loadHeader();
    auto sourceAccountEntry = loadSourceAccount(ltxSource, header);
    auto const& sourceAccount = sourceAccountEntry.current().data.account();

    // Check if the source account doesn't require authorization check
    // Only valid for earlier versions.
    if (protocolVersionIsBefore(header.current().ledgerVersion,
                                ProtocolVersion::V_16) &&
        !(sourceAccount.flags & AUTH_REQUIRED_FLAG))
    {
        innerResult(res).code(ALLOW_TRUST_TRUST_NOT_REQUIRED);
        return false;
    }

    // Check if the source has the authorization to revoke access
    authRevocable = sourceAccount.flags & AUTH_REVOCABLE_FLAG;
    if (!authRevocable && mAllowTrust.authorize == 0)
    {
        innerResult(res).code(ALLOW_TRUST_CANT_REVOKE);
        return false;
    }

    return true;
}

bool
AllowTrustOpFrame::isRevocationToMaintainLiabilitiesValid(
    bool authRevocable, LedgerTxnEntry const& trust, uint32_t flags,
    OperationResult& res) const
{
    // There are two cases where we set the result to
    // ALLOW_TRUST_CANT_REVOKE -
    // 1. We try to revoke authorization when AUTH_REVOCABLE_FLAG is not set
    // (This is handled in isAuthRevocationValid)
    // 2. We try to go from AUTHORIZED_FLAG to
    // AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG when AUTH_REVOCABLE_FLAG is
    // not set. This is handled here.
    if (!authRevocable && (isAuthorized(trust) &&
                           (flags & AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG)))
    {
        innerResult(res).code(ALLOW_TRUST_CANT_REVOKE);
        return false;
    }
    return true;
}

bool
AllowTrustOpFrame::doCheckValid(uint32_t ledgerVersion,
                                OperationResult& res) const
{
    if (mAllowTrust.asset.type() == ASSET_TYPE_NATIVE)
    {
        innerResult(res).code(ALLOW_TRUST_MALFORMED);
        return false;
    }

    if (mAllowTrust.authorize > AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG)
    {
        innerResult(res).code(ALLOW_TRUST_MALFORMED);
        return false;
    }

    if (!trustLineFlagIsValid(mAllowTrust.authorize, ledgerVersion))
    {
        innerResult(res).code(ALLOW_TRUST_MALFORMED);
        return false;
    }

    if (!isAssetValid(mAsset, ledgerVersion))
    {
        innerResult(res).code(ALLOW_TRUST_MALFORMED);
        return false;
    }

    if (protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_16) &&
        mAllowTrust.trustor == getSourceID())
    {
        innerResult(res).code(ALLOW_TRUST_MALFORMED);
        return false;
    }

    return true;
}
}
