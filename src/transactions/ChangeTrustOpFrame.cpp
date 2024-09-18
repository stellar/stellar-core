// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ChangeTrustOpFrame.h"
#include "database/Database.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/TrustLineWrapper.h"
#include "main/Application.h"
#include "transactions/SponsorshipUtils.h"
#include "transactions/TransactionUtils.h"
#include "util/ProtocolVersion.h"
#include <Tracy.hpp>

namespace stellar
{

void
ChangeTrustOpFrame::managePoolOnDeletedTrustLine(
    AbstractLedgerTxn& ltx, TrustLineAsset const& tlAsset) const
{
    LedgerTxn ltxInner(ltx);

    if (tlAsset.type() != ASSET_TYPE_POOL_SHARE)
    {
        return;
    }

    auto const& cp = mChangeTrust.line.liquidityPool().constantProduct();

    decrementLiquidityPoolUseCount(ltxInner, cp.assetA, getSourceID());
    decrementLiquidityPoolUseCount(ltxInner, cp.assetB, getSourceID());

    auto poolLtxEntry = loadLiquidityPool(ltxInner, tlAsset.liquidityPoolID());
    if (!poolLtxEntry)
    {
        throw std::runtime_error("liquidity pool is missing");
    }

    decrementPoolSharesTrustLineCount(poolLtxEntry);

    ltxInner.commit();
}

bool
ChangeTrustOpFrame::tryIncrementPoolUseCount(AbstractLedgerTxn& ltx,
                                             Asset const& asset,
                                             OperationResult& res) const
{
    if (!isIssuer(getSourceID(), asset) && asset.type() != ASSET_TYPE_NATIVE)
    {
        auto assetTrustLine = ltx.load(trustlineKey(getSourceID(), asset));
        if (!assetTrustLine)
        {
            innerResult(res).code(CHANGE_TRUST_TRUST_LINE_MISSING);
            return false;
        }

        if (!isAuthorizedToMaintainLiabilities(assetTrustLine))
        {
            innerResult(res).code(CHANGE_TRUST_NOT_AUTH_MAINTAIN_LIABILITIES);
            return false;
        }

        if (prepareTrustLineEntryExtensionV2(
                assetTrustLine.current().data.trustLine())
                .liquidityPoolUseCount == INT32_MAX)
        {
            throw std::runtime_error("liquidityPoolUseCount is INT32_MAX");
        }

        ++prepareTrustLineEntryExtensionV2(
              assetTrustLine.current().data.trustLine())
              .liquidityPoolUseCount;
    }

    return true;
}

bool
ChangeTrustOpFrame::tryManagePoolOnNewTrustLine(AbstractLedgerTxn& ltx,
                                                TrustLineAsset const& tlAsset,
                                                OperationResult& res) const
{
    LedgerTxn ltxInner(ltx);

    if (tlAsset.type() != ASSET_TYPE_POOL_SHARE)
    {
        return true;
    }

    auto const& cpParams = mChangeTrust.line.liquidityPool().constantProduct();
    if (!tryIncrementPoolUseCount(ltxInner, cpParams.assetA, res) ||
        !tryIncrementPoolUseCount(ltxInner, cpParams.assetB, res))
    {
        return false;
    }

    auto poolLtxEntry = loadLiquidityPool(ltxInner, tlAsset.liquidityPoolID());
    if (poolLtxEntry)
    {
        auto& cp =
            poolLtxEntry.current().data.liquidityPool().body.constantProduct();
        if (cp.poolSharesTrustLineCount == INT64_MAX)
        {
            throw std::runtime_error("poolSharesTrustLineCount is INT64_MAX");
        }

        ++cp.poolSharesTrustLineCount;
    }
    else
    {
        LedgerEntry liquidityPoolEntry;
        liquidityPoolEntry.data.type(LIQUIDITY_POOL);
        auto& lp = liquidityPoolEntry.data.liquidityPool();
        lp.liquidityPoolID = tlAsset.liquidityPoolID();

        auto& cp = lp.body.constantProduct();
        cp.reserveA = 0;
        cp.reserveB = 0;
        cp.totalPoolShares = 0;
        cp.poolSharesTrustLineCount = 1;
        cp.params = cpParams;

        ltxInner.create(liquidityPoolEntry);
    }

    ltxInner.commit();

    return true;
}

ChangeTrustOpFrame::ChangeTrustOpFrame(Operation const& op,
                                       TransactionFrame const& parentTx)
    : OperationFrame(op, parentTx)
    , mChangeTrust(mOperation.body.changeTrustOp())
{
}

bool
ChangeTrustOpFrame::doApply(AppConnector& app, AbstractLedgerTxn& ltx,
                            Hash const& sorobanBasePrngSeed,
                            OperationResult& res,
                            std::shared_ptr<SorobanTxData> sorobanData) const
{
    ZoneNamedN(applyZone, "ChangeTrustOp apply", true);

    // This can be true at this point pre V10
    if (mChangeTrust.line.type() == ASSET_TYPE_NATIVE)
    {
        throw std::runtime_error("native asset is not valid in ChangeTrustOp");
    }

    if (protocolVersionStartsFrom(ltx.loadHeader().current().ledgerVersion,
                                  ProtocolVersion::V_3))
    {
        // Note: No longer checking if issuer exists here, because if
        //     issuerID == getSourceID()
        // and issuer does not exist then this operation would have already
        // failed with opNO_ACCOUNT.
        if (isIssuer(getSourceID(), mChangeTrust.line))
        {
            // since version 3 it is not allowed to use CHANGE_TRUST on self
            innerResult(res).code(CHANGE_TRUST_SELF_NOT_ALLOWED);
            return false;
        }
    }
    else if (isIssuer(getSourceID(), mChangeTrust.line))
    {
        if (mChangeTrust.limit < INT64_MAX)
        {
            innerResult(res).code(CHANGE_TRUST_INVALID_LIMIT);
            return false;
        }
        else if (!loadAccountWithoutRecord(ltx, getSourceID()))
        {
            innerResult(res).code(CHANGE_TRUST_NO_ISSUER);
            return false;
        }
        return true;
    }

    auto const tlAsset = changeTrustAssetToTrustLineAsset(mChangeTrust.line);
    auto const key = trustlineKey(getSourceID(), tlAsset);

    auto trustLine = ltx.load(key);
    if (trustLine)
    { // we are modifying an old trustline
        if (mChangeTrust.limit < getMinimumLimit(ltx.loadHeader(), trustLine))
        {
            // Can't drop the limit below the balance you are holding with them
            innerResult(res).code(CHANGE_TRUST_INVALID_LIMIT);
            return false;
        }

        if (mChangeTrust.limit == 0)
        { // we are deleting a trustline
            // use a lambda so we don't hold a reference to the TrustLineEntry
            auto tlEntry = [&]() -> TrustLineEntry const& {
                return trustLine.current().data.trustLine();
            };

            bool isPoolShare = tlEntry().asset.type() == ASSET_TYPE_POOL_SHARE;

            if (!isPoolShare && hasTrustLineEntryExtV2(tlEntry()) &&
                tlEntry().ext.v1().ext.v2().liquidityPoolUseCount != 0)
            {
                innerResult(res).code(CHANGE_TRUST_CANNOT_DELETE);
                return false;
            }

            // line gets deleted. first release reserves
            auto header = ltx.loadHeader();
            auto sourceAccount = loadSourceAccount(ltx, header);
            removeEntryWithPossibleSponsorship(ltx, header, trustLine.current(),
                                               sourceAccount);
            trustLine.erase();

            // this will create a child LedgerTxn and deactivate all loaded
            // entries!
            managePoolOnDeletedTrustLine(ltx, tlAsset);
        }
        else
        {
            if (mChangeTrust.line.type() != ASSET_TYPE_POOL_SHARE &&
                !loadAccountWithoutRecord(ltx, getIssuer(mChangeTrust.line)))
            {
                innerResult(res).code(CHANGE_TRUST_NO_ISSUER);
                return false;
            }
            trustLine.current().data.trustLine().limit = mChangeTrust.limit;
        }
        innerResult(res).code(CHANGE_TRUST_SUCCESS);
        return true;
    }
    else
    { // new trust line
        if (mChangeTrust.limit == 0)
        {
            innerResult(res).code(CHANGE_TRUST_INVALID_LIMIT);
            return false;
        }

        LedgerEntry trustLineEntry;
        trustLineEntry.data.type(TRUSTLINE);
        auto& tl = trustLineEntry.data.trustLine();
        tl.accountID = getSourceID();
        tl.asset = tlAsset;
        tl.limit = mChangeTrust.limit;
        tl.balance = 0;

        if (mChangeTrust.line.type() != ASSET_TYPE_POOL_SHARE)
        {
            auto issuer =
                loadAccountWithoutRecord(ltx, getIssuer(mChangeTrust.line));
            if (!issuer)
            {
                innerResult(res).code(CHANGE_TRUST_NO_ISSUER);
                return false;
            }
            if (!isAuthRequired(issuer))
            {
                tl.flags = AUTHORIZED_FLAG;
            }
            if (isClawbackEnabledOnAccount(issuer))
            {
                tl.flags |= TRUSTLINE_CLAWBACK_ENABLED_FLAG;
            }
        }

        // this will create a child LedgerTxn and deactivate all loaded
        // entries!
        if (!tryManagePoolOnNewTrustLine(ltx, tlAsset, res))
        {
            return false;
        }

        auto header = ltx.loadHeader();
        auto sourceAccount = loadSourceAccount(ltx, header);
        switch (createEntryWithPossibleSponsorship(ltx, header, trustLineEntry,
                                                   sourceAccount))
        {
        case SponsorshipResult::SUCCESS:
            break;
        case SponsorshipResult::LOW_RESERVE:
            innerResult(res).code(CHANGE_TRUST_LOW_RESERVE);
            return false;
        case SponsorshipResult::TOO_MANY_SUBENTRIES:
            res.code(opTOO_MANY_SUBENTRIES);
            return false;
        case SponsorshipResult::TOO_MANY_SPONSORING:
            res.code(opTOO_MANY_SPONSORING);
            return false;
        case SponsorshipResult::TOO_MANY_SPONSORED:
            // This is impossible right now because there is a limit on sub
            // entries, fall through and throw
        default:
            throw std::runtime_error("Unexpected result from "
                                     "createEntryWithPossibleSponsorship");
        }
        ltx.create(trustLineEntry);

        innerResult(res).code(CHANGE_TRUST_SUCCESS);
        return true;
    }
}

bool
ChangeTrustOpFrame::doCheckValid(uint32_t ledgerVersion,
                                 OperationResult& res) const
{
    if (mChangeTrust.limit < 0)
    {
        innerResult(res).code(CHANGE_TRUST_MALFORMED);
        return false;
    }

    if (!isAssetValid(mChangeTrust.line, ledgerVersion))
    {
        innerResult(res).code(CHANGE_TRUST_MALFORMED);
        return false;
    }
    if (protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_10))
    {
        if (mChangeTrust.line.type() == ASSET_TYPE_NATIVE)
        {
            innerResult(res).code(CHANGE_TRUST_MALFORMED);
            return false;
        }
    }

    if (protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_16) &&
        isIssuer(getSourceID(), mChangeTrust.line))
    {
        innerResult(res).code(CHANGE_TRUST_MALFORMED);
        return false;
    }
    return true;
}
}
