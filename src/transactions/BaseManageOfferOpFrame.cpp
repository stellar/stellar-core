// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/BaseManageOfferOpFrame.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/TrustLineWrapper.h"
#include "transactions/OfferExchange.h"
#include "transactions/TransactionUtils.h"

namespace stellar
{

BaseManageOfferOpFrame::BaseManageOfferOpFrame(
    Operation const& op, OperationResult& res, TransactionFrame& parentTx,
    Asset const& sheep, Asset const& wheat, uint64_t offerID,
    Price const& price, bool passive)
    : OperationFrame(op, res, parentTx)
    , mSheep(sheep)
    , mWheat(wheat)
    , mOfferID(offerID)
    , mPrice(price)
    , mPassive(passive)
{
}

bool
BaseManageOfferOpFrame::checkOfferValid(AbstractLedgerTxn& ltxOuter)
{
    LedgerTxn ltx(ltxOuter); // ltx will always be rolled back

    if (isDeleteOffer())
    {
        // don't bother loading trust lines as we're deleting the offer
        return true;
    }

    if (mSheep.type() != ASSET_TYPE_NATIVE)
    {
        auto sheepLineA = loadTrustLine(ltx, getSourceID(), mSheep);
        auto issuer = stellar::loadAccount(ltx, getIssuer(mSheep));
        if (!issuer)
        {
            setResultSellNoIssuer();
            return false;
        }
        if (!sheepLineA)
        { // we don't have what we are trying to sell
            setResultSellNoTrust();
            return false;
        }
        if (sheepLineA.getBalance() == 0)
        {
            setResultUnderfunded();
            return false;
        }
        if (!sheepLineA.isAuthorized())
        {
            // we are not authorized to sell
            setResultSellNotAuthorized();
            return false;
        }
    }

    if (mWheat.type() != ASSET_TYPE_NATIVE)
    {
        auto wheatLineA = loadTrustLine(ltx, getSourceID(), mWheat);
        auto issuer = stellar::loadAccount(ltx, getIssuer(mWheat));
        if (!issuer)
        {
            setResultBuyNoIssuer();
            return false;
        }
        if (!wheatLineA)
        { // we can't hold what we are trying to buy
            setResultBuyNoTrust();
            return false;
        }
        if (!wheatLineA.isAuthorized())
        { // we are not authorized to hold what we
            // are trying to buy
            setResultBuyNotAuthorized();
            return false;
        }
    }

    return true;
}

bool
BaseManageOfferOpFrame::computeOfferExchangeParameters(
    AbstractLedgerTxn& ltxOuter, bool creatingNewOffer, int64_t& maxSheepSend,
    int64_t& maxWheatReceive)
{
    LedgerTxn ltx(ltxOuter); // ltx will always be rolled back

    auto header = ltx.loadHeader();
    auto sourceAccount = stellar::loadAccount(ltx, getSourceID());

    auto ledgerVersion = header.current().ledgerVersion;

    // Same condition for ManageOfferOp and ManageBuyOfferOp. We always have
    // ledgerVersion >= 10 for ManageBuyOfferOp.
    if (creatingNewOffer &&
        (ledgerVersion >= 10 ||
         (mSheep.type() == ASSET_TYPE_NATIVE && ledgerVersion > 8)))
    {
        // we need to compute maxAmountOfSheepCanSell based on the
        // updated reserve to avoid selling too many and falling
        // below the reserve when we try to create the offer later on
        if (!addNumEntries(header, sourceAccount, 1))
        {
            setResultLowReserve();
            return false;
        }
    }

    auto sheepLineA = loadTrustLineIfNotNative(ltx, getSourceID(), mSheep);
    auto wheatLineA = loadTrustLineIfNotNative(ltx, getSourceID(), mWheat);

    maxWheatReceive = canBuyAtMost(header, sourceAccount, mWheat, wheatLineA);
    maxSheepSend = canSellAtMost(header, sourceAccount, mSheep, sheepLineA);
    if (ledgerVersion >= 10)
    {
        int64_t availableLimit =
            (mWheat.type() == ASSET_TYPE_NATIVE)
                ? getMaxAmountReceive(header, sourceAccount)
                : wheatLineA.getMaxAmountReceive(header);
        if (availableLimit < getOfferBuyingLiabilities())
        {
            setResultLineFull();
            return false;
        }

        int64_t availableBalance =
            (mSheep.type() == ASSET_TYPE_NATIVE)
                ? getAvailableBalance(header, sourceAccount)
                : sheepLineA.getAvailableBalance(header);
        if (availableBalance < getOfferSellingLiabilities())
        {
            setResultUnderfunded();
            return false;
        }

        applyOperationSpecificLimits(maxSheepSend, 0, maxWheatReceive, 0);
    }
    else
    {
        getExchangeParametersBeforeV10(maxSheepSend, maxWheatReceive);
    }

    return true;
}

bool
BaseManageOfferOpFrame::doApply(AbstractLedgerTxn& ltxOuter)
{
    LedgerTxn ltx(ltxOuter);
    if (!checkOfferValid(ltx))
    {
        return false;
    }

    bool creatingNewOffer = false;
    bool passive = false;
    int64_t amount = 0;
    uint32_t flags = 0;

    if (mOfferID)
    { // modifying an old offer
        auto sellSheepOffer = stellar::loadOffer(ltx, getSourceID(), mOfferID);
        if (!sellSheepOffer)
        {
            setResultNotFound();
            return false;
        }

        // We are releasing the liabilites associated with this offer. This is
        // required in order to produce available balance for the offer to be
        // executed. Both trust lines must be reset since it is possible that
        // the assets are updated (including the edge case that the buying and
        // selling assets are swapped).
        auto header = ltx.loadHeader();
        if (header.current().ledgerVersion >= 10)
        {
            releaseLiabilities(ltx, header, sellSheepOffer);
        }

        // Capture flags state before erasing offer
        flags = sellSheepOffer.current().data.offer().flags;
        passive = flags & PASSIVE_FLAG;

        // WARNING: sellSheepOffer is deleted but sourceAccount is not updated
        // to reflect the change in numSubEntries at this point. However, we
        // can't update it here since doing so would modify sourceAccount,
        // which would lead to different buckets being generated.
        sellSheepOffer.erase();
    }
    else
    { // creating a new Offer
        creatingNewOffer = true;
        passive = mPassive;
        flags = passive ? PASSIVE_FLAG : 0;
    }

    setResultSuccess();

    if (!isDeleteOffer())
    {
        int64_t maxSheepSend = 0;
        int64_t maxWheatReceive = 0;
        if (!computeOfferExchangeParameters(ltx, creatingNewOffer, maxSheepSend,
                                            maxWheatReceive))
        {
            return false;
        }

        // Make sure that we can actually receive something.
        if (maxWheatReceive == 0)
        {
            setResultLineFull();
            return false;
        }

        int64_t sheepSent, wheatReceived;
        std::vector<ClaimOfferAtom> offerTrail;
        Price maxWheatPrice(mPrice.d, mPrice.n);
        ConvertResult r = convertWithOffers(
            ltx, mSheep, maxSheepSend, sheepSent, mWheat, maxWheatReceive,
            wheatReceived, false,
            [this, passive, &maxWheatPrice](LedgerTxnEntry const& entry) {
                auto const& o = entry.current().data.offer();
                assert(o.offerID != mOfferID);
                if ((passive && (o.price >= maxWheatPrice)) ||
                    (o.price > maxWheatPrice))
                {
                    return OfferFilterResult::eStop;
                }
                if (o.sellerID == getSourceID())
                {
                    // we are crossing our own offer
                    setResultCrossSelf();
                    return OfferFilterResult::eStop;
                }
                return OfferFilterResult::eKeep;
            },
            offerTrail);
        assert(sheepSent >= 0);

        bool sheepStays;
        switch (r)
        {
        case ConvertResult::eOK:
            sheepStays = false;
            break;
        case ConvertResult::ePartial:
            sheepStays = true;
            break;
        case ConvertResult::eFilterStop:
            if (!isResultSuccess())
            {
                return false;
            }
            sheepStays = true;
            break;
        default:
            abort();
        }

        // updates the result with the offers that got taken on the way
        for (auto const& oatom : offerTrail)
        {
            getSuccessResult().offersClaimed.push_back(oatom);
        }

        auto header = ltx.loadHeader();
        if (wheatReceived > 0)
        {
            if (mWheat.type() == ASSET_TYPE_NATIVE)
            {
                auto sourceAccount = stellar::loadAccount(ltx, getSourceID());
                if (!addBalance(header, sourceAccount, wheatReceived))
                {
                    // this would indicate a bug in OfferExchange
                    throw std::runtime_error("offer claimed over limit");
                }
            }
            else
            {
                auto wheatLineA = loadTrustLine(ltx, getSourceID(), mWheat);
                if (!wheatLineA.addBalance(header, wheatReceived))
                {
                    // this would indicate a bug in OfferExchange
                    throw std::runtime_error("offer claimed over limit");
                }
            }

            if (mSheep.type() == ASSET_TYPE_NATIVE)
            {
                auto sourceAccount = stellar::loadAccount(ltx, getSourceID());
                if (!addBalance(header, sourceAccount, -sheepSent))
                {
                    // this would indicate a bug in OfferExchange
                    throw std::runtime_error("offer sold more than balance");
                }
            }
            else
            {
                auto sheepLineA = loadTrustLine(ltx, getSourceID(), mSheep);
                if (!sheepLineA.addBalance(header, -sheepSent))
                {
                    // this would indicate a bug in OfferExchange
                    throw std::runtime_error("offer sold more than balance");
                }
            }
        }

        if (header.current().ledgerVersion >= 10)
        {
            if (sheepStays)
            {
                auto sourceAccount =
                    stellar::loadAccountWithoutRecord(ltx, getSourceID());
                auto sheepLineA = loadTrustLineWithoutRecordIfNotNative(
                    ltx, getSourceID(), mSheep);
                auto wheatLineA = loadTrustLineWithoutRecordIfNotNative(
                    ltx, getSourceID(), mWheat);

                int64_t sheepSendLimit =
                    canSellAtMost(header, sourceAccount, mSheep, sheepLineA);
                int64_t wheatReceiveLimit =
                    canBuyAtMost(header, sourceAccount, mWheat, wheatLineA);
                applyOperationSpecificLimits(sheepSendLimit, sheepSent,
                                             wheatReceiveLimit, wheatReceived);
                amount = adjustOffer(mPrice, sheepSendLimit, wheatReceiveLimit);
            }
            else
            {
                amount = 0;
            }
        }
        else
        {
            amount = maxSheepSend - sheepSent;
        }
    }

    auto header = ltx.loadHeader();
    if (amount > 0)
    {
        auto newOffer = buildOffer(amount, flags);
        if (creatingNewOffer)
        {
            auto sourceAccount = stellar::loadAccount(ltx, getSourceID());
            if (!addNumEntries(header, sourceAccount, 1))
            {
                setResultLowReserve();
                return false;
            }

            newOffer.data.offer().offerID = generateID(header);
            getSuccessResult().offer.effect(MANAGE_OFFER_CREATED);
        }
        else
        {
            getSuccessResult().offer.effect(MANAGE_OFFER_UPDATED);
        }

        auto sellSheepOffer = ltx.create(newOffer);
        getSuccessResult().offer.offer() =
            sellSheepOffer.current().data.offer();

        if (header.current().ledgerVersion >= 10)
        {
            acquireLiabilities(ltx, header, sellSheepOffer);
        }
    }
    else
    {
        getSuccessResult().offer.effect(MANAGE_OFFER_DELETED);

        if (!creatingNewOffer)
        {
            auto sourceAccount = stellar::loadAccount(ltx, getSourceID());
            addNumEntries(header, sourceAccount, -1);
        }
    }

    ltx.commit();
    return true;
}

LedgerEntry
BaseManageOfferOpFrame::buildOffer(int64_t amount, uint32_t flags) const
{
    LedgerEntry le;
    le.data.type(OFFER);
    OfferEntry& o = le.data.offer();

    o.sellerID = getSourceID();
    o.amount = amount;
    o.price = mPrice;
    o.offerID = mOfferID;
    o.selling = mSheep;
    o.buying = mWheat;
    o.flags = flags;
    return le;
}
}
