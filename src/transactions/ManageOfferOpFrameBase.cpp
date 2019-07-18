// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/ManageOfferOpFrameBase.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/TrustLineWrapper.h"
#include "transactions/OfferExchange.h"
#include "transactions/TransactionUtils.h"

namespace stellar
{

ManageOfferOpFrameBase::ManageOfferOpFrameBase(
    Operation const& op, OperationResult& res, TransactionFrame& parentTx,
    Asset const& sheep, Asset const& wheat, int64_t offerID, Price const& price,
    bool setPassiveOnCreate)
    : OperationFrame(op, res, parentTx)
    , mSheep(sheep)
    , mWheat(wheat)
    , mOfferID(offerID)
    , mPrice(price)
    , mSetPassiveOnCreate(setPassiveOnCreate)
{
}

bool
ManageOfferOpFrameBase::checkOfferValid(AbstractLedgerTxn& ltxOuter)
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
ManageOfferOpFrameBase::computeOfferExchangeParameters(
    AbstractLedgerTxn& ltxOuter, bool creatingNewOffer, int64_t& maxSheepSend,
    int64_t& maxWheatReceive)
{
    LedgerTxn ltx(ltxOuter); // ltx will always be rolled back

    auto header = ltx.loadHeader();
    auto sourceAccount = loadSourceAccount(ltx, header);

    auto ledgerVersion = header.current().ledgerVersion;

    if (creatingNewOffer &&
        (ledgerVersion >= 10 ||
         (mSheep.type() == ASSET_TYPE_NATIVE && ledgerVersion > 8)))
    {
        // we need to compute maxAmountOfSheepCanSell based on the
        // updated reserve to avoid selling too many and falling
        // below the reserve when we try to create the offer later on
        switch (addNumEntries(header, sourceAccount, 1))
        {
        case AddSubentryResult::SUCCESS:
            break;
        case AddSubentryResult::LOW_RESERVE:
            setResultLowReserve();
            return false;
        case AddSubentryResult::TOO_MANY_SUBENTRIES:
            mResult.code(opTOO_MANY_SUBENTRIES);
            return false;
        default:
            throw std::runtime_error("Unexpected result from addNumEntries");
        }
    }

    auto sheepLineA = loadTrustLineIfNotNative(ltx, getSourceID(), mSheep);
    auto wheatLineA = loadTrustLineIfNotNative(ltx, getSourceID(), mWheat);

    maxWheatReceive = canBuyAtMost(header, sourceAccount, mWheat, wheatLineA);
    maxSheepSend = canSellAtMost(header, sourceAccount, mSheep, sheepLineA);
    if (ledgerVersion >= 10)
    {
        // Note that maxWheatReceive = max(0, availableLimit). But why do we
        // work with availableLimit?
        // - If availableLimit >= 0 then maxWheatReceive = availableLimit so
        //   they are interchangeable.
        // - If availableLimit < 0 then maxWheatReceive = 0. But if
        //   getOfferBuyingLiabilities() == 0 (which is possible) then we have
        //       availableLimit < 0 = getOfferBuyingLiabilities()
        //       maxWheatReceive = 0 = getOfferBuyingLiailities()
        //   which are not the same. Using availableLimit allows us to return
        //   LINE_FULL here in cases where the availableLimit is negative. This
        //   makes sense: all we are checking here is that liabilities fit into
        //   the available limit, and no liabilities fit if the available limit
        //   is negative.
        // In practice, I _think_ that negative available limit is not possible
        // unless there is a logic error.
        int64_t availableLimit =
            (mWheat.type() == ASSET_TYPE_NATIVE)
                ? getMaxAmountReceive(header, sourceAccount)
                : wheatLineA.getMaxAmountReceive(header);
        if (availableLimit < getOfferBuyingLiabilities())
        {
            setResultLineFull();
            return false;
        }

        // Note that maxSheepSend = max(0, availableBalance). But why do we
        // work with availableBalance?
        // - If availableBalance >= 0 then maxSheepSend = availableBalance so
        //   they are interchangeable.
        // - If availableBalance < 0 then maxSheepSend = 0. But if
        //   getOfferSellingLiabilities() == 0 (which is possible) then we have
        //       availableBalance < 0 = getOfferSellingLiabilities()
        //       maxSheepSend = 0 = getOfferSellingLiailities()
        //   which are not the same. Using availableBalance allows us to return
        //   LINE_FULL here in cases where the availableBalance is negative.
        //   This makes sense: all we are checking here is that liabilities fit
        //   into the available balance, and no liabilities fit if the available
        //   balance is negative.
        // In practice, negative available balance is possible for native assets
        // after the reserve has been raised.
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
ManageOfferOpFrameBase::doApply(AbstractLedgerTxn& ltxOuter)
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
        passive = mSetPassiveOnCreate;
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

        int64_t maxOffersToCross = INT64_MAX;
        if (ltx.loadHeader().current().ledgerVersion >=
            FIRST_PROTOCOL_SUPPORTING_OPERATION_LIMITS)
        {
            maxOffersToCross = MAX_OFFERS_TO_CROSS;
        }

        int64_t sheepSent, wheatReceived;
        std::vector<ClaimOfferAtom> offerTrail;
        Price maxWheatPrice(mPrice.d, mPrice.n);
        ConvertResult r = convertWithOffers(
            ltx, mSheep, maxSheepSend, sheepSent, mWheat, maxWheatReceive,
            wheatReceived, RoundingType::NORMAL,
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
            offerTrail, maxOffersToCross);
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
        case ConvertResult::eCrossedTooMany:
            mResult.code(opEXCEEDED_WORK_LIMIT);
            return false;
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
                auto sourceAccount = loadSourceAccount(ltx, header);
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
                auto sourceAccount = loadSourceAccount(ltx, header);
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
            // make sure we don't allow us to add offers when we don't have
            // the minbalance (should never happen at this stage in v9+)
            auto sourceAccount = loadSourceAccount(ltx, header);
            switch (addNumEntries(header, sourceAccount, 1))
            {
            case AddSubentryResult::SUCCESS:
                break;
            case AddSubentryResult::LOW_RESERVE:
                setResultLowReserve();
                return false;
            case AddSubentryResult::TOO_MANY_SUBENTRIES:
                mResult.code(opTOO_MANY_SUBENTRIES);
                return false;
            default:
                throw std::runtime_error(
                    "Unexpected result from addNumEntries");
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
            auto sourceAccount = loadSourceAccount(ltx, header);
            addNumEntries(header, sourceAccount, -1);
        }
    }

    ltx.commit();
    return true;
}

LedgerEntry
ManageOfferOpFrameBase::buildOffer(int64_t amount, uint32_t flags) const
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

// makes sure the currencies are different
bool
ManageOfferOpFrameBase::doCheckValid(uint32_t ledgerVersion)
{
    if (!isAssetValid(mSheep) || !isAssetValid(mWheat))
    {
        setResultMalformed();
        return false;
    }
    if (compareAsset(mSheep, mWheat))
    {
        setResultMalformed();
        return false;
    }

    if (!isAmountValid() || mPrice.d <= 0 || mPrice.n <= 0)
    {
        setResultMalformed();
        return false;
    }

    if (mOfferID == 0 && isDeleteOffer())
    {
        if (ledgerVersion >= 11)
        {
            setResultMalformed();
            return false;
        }
        else if (ledgerVersion >= 3)
        {
            setResultNotFound();
            return false;
        }
        // Note: This was not invalid before version 3
    }

    return true;
}

void
ManageOfferOpFrameBase::insertLedgerKeysToPrefetch(
    std::unordered_set<LedgerKey>& keys) const
{
    if (isDeleteOffer())
    {
        return;
    }

    // Prefetch existing offer
    if (mOfferID)
    {
        keys.emplace(offerKey(getSourceID(), mOfferID));
    }

    auto addIssuerAndTrustline = [&](Asset const& asset) {
        if (asset.type() != ASSET_TYPE_NATIVE)
        {
            auto issuer = getIssuer(asset);
            keys.emplace(accountKey(issuer));
            keys.emplace(trustlineKey(this->getSourceID(), asset));
        }
    };

    addIssuerAndTrustline(mSheep);
    addIssuerAndTrustline(mWheat);
}
}
