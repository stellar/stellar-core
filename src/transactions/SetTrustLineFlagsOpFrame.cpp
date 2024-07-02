// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/SetTrustLineFlagsOpFrame.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "main/Application.h"
#include "transactions/TransactionUtils.h"
#include "util/ProtocolVersion.h"
#include <Tracy.hpp>

namespace stellar
{

SetTrustLineFlagsOpFrame::SetTrustLineFlagsOpFrame(
    Operation const& op, TransactionFrame const& parentTx, uint32_t index)
    : TrustFlagsOpFrameBase(op, parentTx)
    , mSetTrustLineFlags(mOperation.body.setTrustLineFlagsOp())
    , mOpIndex(index)
{
}

bool
SetTrustLineFlagsOpFrame::isOpSupported(LedgerHeader const& header) const
{
    return protocolVersionStartsFrom(header.ledgerVersion,
                                     ProtocolVersion::V_17);
}

void
SetTrustLineFlagsOpFrame::setResultSelfNotAllowed(OperationResult& res) const
{
    throw std::runtime_error("Not implemented.");
}

void
SetTrustLineFlagsOpFrame::setResultNoTrustLine(OperationResult& res) const
{
    innerResult(res).code(SET_TRUST_LINE_FLAGS_NO_TRUST_LINE);
}

void
SetTrustLineFlagsOpFrame::setResultLowReserve(OperationResult& res) const
{
    innerResult(res).code(SET_TRUST_LINE_FLAGS_LOW_RESERVE);
}

void
SetTrustLineFlagsOpFrame::setResultSuccess(OperationResult& res) const
{
    innerResult(res).code(SET_TRUST_LINE_FLAGS_SUCCESS);
}

AccountID const&
SetTrustLineFlagsOpFrame::getOpTrustor() const
{
    return mSetTrustLineFlags.trustor;
}

Asset const&
SetTrustLineFlagsOpFrame::getOpAsset() const
{
    return mSetTrustLineFlags.asset;
}

uint32_t
SetTrustLineFlagsOpFrame::getOpIndex() const
{
    return mOpIndex;
}

bool
SetTrustLineFlagsOpFrame::calcExpectedFlagValue(LedgerTxnEntry const& trust,
                                                uint32_t& expectedVal,
                                                OperationResult& res) const
{
    expectedVal = trust.current().data.trustLine().flags;
    expectedVal &= ~mSetTrustLineFlags.clearFlags;
    expectedVal |= mSetTrustLineFlags.setFlags;

    if (!trustLineFlagAuthIsValid(expectedVal))
    {
        innerResult(res).code(SET_TRUST_LINE_FLAGS_INVALID_STATE);
        return false;
    }
    return true;
}

void
SetTrustLineFlagsOpFrame::setFlagValue(AbstractLedgerTxn& ltx,
                                       LedgerKey const& key,
                                       uint32_t flagVal) const
{
    auto trust = ltx.load(key);
    trust.current().data.trustLine().flags = flagVal;
}

bool
SetTrustLineFlagsOpFrame::doCheckValid(uint32_t ledgerVersion,
                                       OperationResult& res) const
{
    if (mSetTrustLineFlags.asset.type() == ASSET_TYPE_NATIVE)
    {
        innerResult(res).code(SET_TRUST_LINE_FLAGS_MALFORMED);
        return false;
    }

    if (!isAssetValid(mSetTrustLineFlags.asset, ledgerVersion))
    {
        innerResult(res).code(SET_TRUST_LINE_FLAGS_MALFORMED);
        return false;
    }

    if (!(getSourceID() == getIssuer(mSetTrustLineFlags.asset)))
    {
        innerResult(res).code(SET_TRUST_LINE_FLAGS_MALFORMED);
        return false;
    }

    if (mSetTrustLineFlags.trustor == getSourceID())
    {
        innerResult(res).code(SET_TRUST_LINE_FLAGS_MALFORMED);
        return false;
    }

    if ((mSetTrustLineFlags.setFlags & mSetTrustLineFlags.clearFlags) != 0)
    {
        innerResult(res).code(SET_TRUST_LINE_FLAGS_MALFORMED);
        return false;
    }

    // setFlags has the same restrictions as the trustline flags, so we can
    // use trustLineFlagIsValid here
    if (!trustLineFlagIsValid(mSetTrustLineFlags.setFlags, ledgerVersion) ||
        (mSetTrustLineFlags.setFlags & TRUSTLINE_CLAWBACK_ENABLED_FLAG) != 0)
    {
        innerResult(res).code(SET_TRUST_LINE_FLAGS_MALFORMED);
        return false;
    }

    if (!trustLineFlagMaskCheckIsValid(mSetTrustLineFlags.clearFlags,
                                       ledgerVersion))
    {
        innerResult(res).code(SET_TRUST_LINE_FLAGS_MALFORMED);
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
SetTrustLineFlagsOpFrame::isAuthRevocationValid(AbstractLedgerTxn& ltx,
                                                bool& authRevocable,
                                                OperationResult& res) const
{

    // Load the source account entry
    LedgerTxn ltxSource(ltx); // ltxSource will be rolled back
    auto header = ltxSource.loadHeader();
    auto sourceAccountEntry = loadSourceAccount(ltxSource, header);

    if ((sourceAccountEntry.current().data.account().flags &
         AUTH_REVOCABLE_FLAG) != 0)
    {
        return true;
    }

    // If the account entry doesn't have "authorization revocable" flag,
    // Check if the operation is still allowed.

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

    bool isValid = !clearingAnyAuth || settingAuthorized;

    if (!isValid)
    {
        innerResult(res).code(SET_TRUST_LINE_FLAGS_CANT_REVOKE);
    }
    return isValid;
}

bool
SetTrustLineFlagsOpFrame::isRevocationToMaintainLiabilitiesValid(
    bool authRevocable, LedgerTxnEntry const& trust, uint32_t flags,
    OperationResult& res) const
{
    // This has already been checked in isAuthRevocationValid,
    // always return true here.
    return true;
}

}
