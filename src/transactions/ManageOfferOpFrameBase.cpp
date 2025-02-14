// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/ManageOfferOpFrameBase.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/TrustLineWrapper.h"
#include "transactions/OfferExchange.h"
#include "transactions/SponsorshipUtils.h"
#include "transactions/TransactionUtils.h"
#include "util/GlobalChecks.h"
#include "util/ProtocolVersion.h"
#include <Tracy.hpp>

namespace stellar
{

ManageOfferOpFrameBase::ManageOfferOpFrameBase(
    Operation const& op, TransactionFrame const& parentTx, Asset const& sheep,
    Asset const& wheat, int64_t offerID, Price const& price,
    bool setPassiveOnCreate)
    : OperationFrame(op, parentTx)
    , mSheep(sheep)
    , mWheat(wheat)
    , mOfferID(offerID)
    , mPrice(price)
    , mSetPassiveOnCreate(setPassiveOnCreate)
{
}

bool
ManageOfferOpFrameBase::checkOfferValid(AbstractLedgerTxn& ltxOuter,
                                        OperationResult& res) const
{
    LedgerTxn ltx(ltxOuter); // ltx will always be rolled back

    if (isDeleteOffer())
    {
        // don't bother loading trust lines as we're deleting the offer
        return true;
    }

    auto ledgerVersion = ltx.loadHeader().current().ledgerVersion;

    if (mSheep.type() != ASSET_TYPE_NATIVE)
    {
        auto sheepLineA = loadTrustLine(ltx, getSourceID(), mSheep);
        if (protocolVersionIsBefore(ledgerVersion, ProtocolVersion::V_13))
        {
            auto issuer = stellar::loadAccount(ltx, getIssuer(mSheep));
            if (!issuer)
            {
                setResultSellNoIssuer(res);
                return false;
            }
        }
        if (!sheepLineA)
        { // we don't have what we are trying to sell
            setResultSellNoTrust(res);
            return false;
        }
        if (sheepLineA.getBalance() == 0)
        {
            setResultUnderfunded(res);
            return false;
        }
        if (!sheepLineA.isAuthorized())
        {
            // we are not authorized to sell
            setResultSellNotAuthorized(res);
            return false;
        }
    }

    if (mWheat.type() != ASSET_TYPE_NATIVE)
    {
        auto wheatLineA = loadTrustLine(ltx, getSourceID(), mWheat);

        if (protocolVersionIsBefore(ledgerVersion, ProtocolVersion::V_13))
        {
            auto issuer = stellar::loadAccount(ltx, getIssuer(mWheat));
            if (!issuer)
            {
                setResultBuyNoIssuer(res);
                return false;
            }
        }
        if (!wheatLineA)
        { // we can't hold what we are trying to buy
            setResultBuyNoTrust(res);
            return false;
        }
        if (!wheatLineA.isAuthorized())
        { // we are not authorized to hold what we
            // are trying to buy
            setResultBuyNotAuthorized(res);
            return false;
        }
    }

    return true;
}

bool
ManageOfferOpFrameBase::computeOfferExchangeParameters(
    AbstractLedgerTxn& ltxOuter, OperationResult& res, bool creatingNewOffer,
    int64_t& maxSheepSend, int64_t& maxWheatReceive) const
{
    LedgerTxn ltx(ltxOuter); // ltx will always be rolled back

    auto header = ltx.loadHeader();
    auto sourceAccount = loadSourceAccount(ltx, header);

    auto ledgerVersion = header.current().ledgerVersion;
    if (protocolVersionIsBefore(ledgerVersion, ProtocolVersion::V_14) &&
        creatingNewOffer &&
        (protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_10) ||
         (mSheep.type() == ASSET_TYPE_NATIVE &&
          protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_9))))
    {
        // we need to compute maxAmountOfSheepCanSell based on the
        // updated reserve to avoid selling too many and falling
        // below the reserve when we try to create the offer later on
        auto le = buildOffer(0, 0, LedgerEntry::_ext_t{});
        switch (canCreateEntryWithoutSponsorship(header.current(), le,
                                                 sourceAccount.current()))
        {
        case SponsorshipResult::SUCCESS:
            break;
        case SponsorshipResult::LOW_RESERVE:
            setResultLowReserve(res);
            return false;
        case SponsorshipResult::TOO_MANY_SUBENTRIES:
            res.code(opTOO_MANY_SUBENTRIES);
            return false;
        default:
            // Includes of SponsorshipResult::TOO_MANY_SPONSORING
            // and SponsorshipResult::TOO_MANY_SPONSORED
            throw std::runtime_error("Unexpected result from "
                                     "createEntryWithPossibleSponsorship");
        }
        createEntryWithoutSponsorship(le, sourceAccount.current());
    }

    auto sheepLineA = loadTrustLineIfNotNative(ltx, getSourceID(), mSheep);
    auto wheatLineA = loadTrustLineIfNotNative(ltx, getSourceID(), mWheat);

    maxWheatReceive = canBuyAtMost(header, sourceAccount, mWheat, wheatLineA);
    maxSheepSend = canSellAtMost(header, sourceAccount, mSheep, sheepLineA);
    if (protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_10))
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
            setResultLineFull(res);
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
            setResultUnderfunded(res);
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
ManageOfferOpFrameBase::doApply(
    AppConnector& app, AbstractLedgerTxn& ltxOuter,
    Hash const& sorobanBasePrngSeed, OperationResult& res,
    std::shared_ptr<SorobanTxData> sorobanData) const
{
    ZoneNamedN(applyZone, "ManageOfferOp apply", true);
    std::string pairStr = assetToString(mSheep);
    pairStr += ":";
    pairStr += assetToString(mWheat);
    ZoneTextV(applyZone, pairStr.c_str(), pairStr.size());

    LedgerTxn ltx(ltxOuter);
    if (!checkOfferValid(ltx, res))
    {
        return false;
    }

    bool creatingNewOffer = false;
    bool passive = false;
    int64_t amount = 0;
    uint32_t flags = 0;
    LedgerEntry::_ext_t extension;

    Price oldSellSheepOfferPrice(Price(0, 0));
    int64_t oldSheepAmount = 0;

    if (mOfferID)
    { // modifying an old offer
        auto sellSheepOffer = stellar::loadOffer(ltx, getSourceID(), mOfferID);
        if (!sellSheepOffer)
        {
            setResultNotFound(res);
            return false;
        }

        oldSellSheepOfferPrice = sellSheepOffer.current().data.offer().price;
        oldSheepAmount = sellSheepOffer.current().data.offer().amount;

        // We are releasing the liabilites associated with this offer. This is
        // required in order to produce available balance for the offer to be
        // executed. Both trust lines must be reset since it is possible that
        // the assets are updated (including the edge case that the buying and
        // selling assets are swapped).
        auto header = ltx.loadHeader();
        if (protocolVersionStartsFrom(header.current().ledgerVersion,
                                      ProtocolVersion::V_10))
        {
            releaseLiabilities(ltx, header, sellSheepOffer);
        }

        // Capture flags state before erasing offer
        flags = sellSheepOffer.current().data.offer().flags;
        passive = flags & PASSIVE_FLAG;

        // Capture sponsorship before erasing offer
        extension = sellSheepOffer.current().ext;

        // WARNING: sellSheepOffer is deleted but sourceAccount is not updated
        // to reflect the change in numSubEntries at this point. However, we
        // can't update it here since doing so would modify sourceAccount,
        // which would lead to different buckets being generated. Furthermore,
        // sponsorships are not updated here.
        //
        // This allows the entire operation to be applied after accounting for
        // the potential of retaining this entry.
        sellSheepOffer.erase();
    }
    else
    { // creating a new Offer
        creatingNewOffer = true;
        passive = mSetPassiveOnCreate;
        flags = passive ? PASSIVE_FLAG : 0;

        auto header = ltx.loadHeader();
        if (protocolVersionStartsFrom(header.current().ledgerVersion,
                                      ProtocolVersion::V_14))
        {
            // WARNING: no offer is actually created here. Instead, we are just
            // establishing the numSubEntries and sponsorship changes.
            //
            // This allows the entire operation to be applied after accounting
            // for potential of creating this entry.
            auto le = buildOffer(0, 0, LedgerEntry::_ext_t{});
            auto sourceAccount = loadAccount(ltx, getSourceID());
            switch (createEntryWithPossibleSponsorship(ltx, header, le,
                                                       sourceAccount))
            {
            case SponsorshipResult::SUCCESS:
                break;
            case SponsorshipResult::LOW_RESERVE:
                setResultLowReserve(res);
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

            extension = le.ext;
        }
    }

    setResultSuccess(res);

    if (!isDeleteOffer())
    {
        int64_t maxSheepSend = 0;
        int64_t maxWheatReceive = 0;
        if (!computeOfferExchangeParameters(ltx, res, creatingNewOffer,
                                            maxSheepSend, maxWheatReceive))
        {
            return false;
        }

        // Make sure that we can actually receive something.
        if (maxWheatReceive == 0)
        {
            setResultLineFull(res);
            return false;
        }

        int64_t maxOffersToCross = INT64_MAX;
        if (protocolVersionStartsFrom(
                ltx.loadHeader().current().ledgerVersion,
                FIRST_PROTOCOL_SUPPORTING_OPERATION_LIMITS))
        {
            maxOffersToCross = getMaxOffersToCross();
        }

        int64_t sheepSent, wheatReceived;
        std::vector<ClaimAtom> offerTrail;
        Price maxWheatPrice(mPrice.d, mPrice.n);
        ConvertResult r = convertWithOffersAndPools(
            ltx, mSheep, maxSheepSend, sheepSent, mWheat, maxWheatReceive,
            wheatReceived, RoundingType::NORMAL,
            [this, passive, &maxWheatPrice](LedgerTxnEntry const& entry) {
                auto const& o = entry.current().data.offer();
                releaseAssertOrThrow(o.offerID != mOfferID);
                if ((passive && (o.price >= maxWheatPrice)) ||
                    (o.price > maxWheatPrice))
                {
                    return OfferFilterResult::eStopBadPrice;
                }
                if (o.sellerID == getSourceID())
                {
                    // we are crossing our own offer
                    return OfferFilterResult::eStopCrossSelf;
                }
                return OfferFilterResult::eKeep;
            },
            offerTrail, maxOffersToCross);

        releaseAssertOrThrow(sheepSent >= 0);

        bool sheepStays;
        switch (r)
        {
        case ConvertResult::eOK:
            sheepStays = false;
            break;
        case ConvertResult::ePartial:
            sheepStays = true;
            break;
        case ConvertResult::eFilterStopBadPrice:
            sheepStays = true;
            break;
        case ConvertResult::eFilterStopCrossSelf:
            setResultCrossSelf(res);
            return false;
        case ConvertResult::eCrossedTooMany:
            res.code(opEXCEEDED_WORK_LIMIT);
            return false;
        default:
            abort();
        }

        // updates the result with the offers that got taken on the way
        for (auto const& oatom : offerTrail)
        {
            getSuccessResult(res).offersClaimed.push_back(oatom);
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

        if (protocolVersionStartsFrom(header.current().ledgerVersion,
                                      ProtocolVersion::V_10))
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
        auto newOffer = buildOffer(amount, flags, extension);
        if (creatingNewOffer)
        {
            if (protocolVersionIsBefore(header.current().ledgerVersion,
                                        ProtocolVersion::V_14))
            {
                // make sure we don't allow us to add offers when we don't have
                // the minbalance (should never happen at this stage in v9+)
                auto sourceAccount = loadSourceAccount(ltx, header);
                switch (canCreateEntryWithoutSponsorship(
                    header.current(), newOffer, sourceAccount.current()))
                {
                case SponsorshipResult::SUCCESS:
                    break;
                case SponsorshipResult::LOW_RESERVE:
                    setResultLowReserve(res);
                    return false;
                case SponsorshipResult::TOO_MANY_SUBENTRIES:
                    res.code(opTOO_MANY_SUBENTRIES);
                    return false;
                default:
                    // Includes of SponsorshipResult::TOO_MANY_SPONSORING
                    // and SponsorshipResult::TOO_MANY_SPONSORED
                    throw std::runtime_error(
                        "Unexpected result from "
                        "createEntryWithPossibleSponsorship");
                }
                createEntryWithoutSponsorship(newOffer,
                                              sourceAccount.current());
            }
            // In versions 14 and beyond, numSubEntries and sponsorships are
            // updated at the beginning of this operation. Therefore, there is
            // nothing to do here.

            newOffer.data.offer().offerID = generateNewOfferID(header);
            getSuccessResult(res).offer.effect(MANAGE_OFFER_CREATED);
        }
        else
        {
            getSuccessResult(res).offer.effect(MANAGE_OFFER_UPDATED);
        }

        auto sellSheepOffer = ltx.create(newOffer);
        getSuccessResult(res).offer.offer() =
            sellSheepOffer.current().data.offer();

        if (protocolVersionStartsFrom(header.current().ledgerVersion,
                                      ProtocolVersion::V_10))
        {
            acquireLiabilities(ltx, header, sellSheepOffer);
        }
    }
    else
    {
        getSuccessResult(res).offer.effect(MANAGE_OFFER_DELETED);

        if (!creatingNewOffer ||
            protocolVersionStartsFrom(header.current().ledgerVersion,
                                      ProtocolVersion::V_14))
        {
            auto sourceAccount = loadSourceAccount(ltx, header);
            auto le = buildOffer(0, 0, extension);
            removeEntryWithPossibleSponsorship(ltx, header, le, sourceAccount);
        }
    }

    ltx.commit();

    // Deleting an offer does not invalidate any cached fails
    if (!isDeleteOffer())
    {
        // Unfortunately, due to rounding errors we need to invalidate both side
        // of the offer. Sometimes, even if an offer gets "worse", (like if the
        // amount offer decreases), different rounding behavior can actually
        // cause the "worse" offer to now favor the taker.
        app.getLedgerManager().invalidatePathPaymentCachesForAssetPair(
            AssetPair{mSheep, mWheat});
        app.getLedgerManager().invalidatePathPaymentCachesForAssetPair(
            AssetPair{mWheat, mSheep});
    }

    return true;
}

bool
ManageOfferOpFrameBase::isDexOperation() const
{
    return !isDeleteOffer();
}

int64_t
ManageOfferOpFrameBase::generateNewOfferID(LedgerTxnHeader& header) const
{
    return generateID(header);
}

LedgerEntry
ManageOfferOpFrameBase::buildOffer(int64_t amount, uint32_t flags,
                                   LedgerEntry::_ext_t const& extension) const
{
    LedgerEntry le;
    le.data.type(OFFER);
    le.ext = extension;

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
ManageOfferOpFrameBase::doCheckValid(uint32_t ledgerVersion,
                                     OperationResult& res) const
{
    if (!isAssetValid(mSheep, ledgerVersion) ||
        !isAssetValid(mWheat, ledgerVersion))
    {
        setResultMalformed(res);
        return false;
    }
    if (compareAsset(mSheep, mWheat))
    {
        setResultMalformed(res);
        return false;
    }

    if (!isAmountValid() || mPrice.d <= 0 || mPrice.n <= 0)
    {
        setResultMalformed(res);
        return false;
    }

    if (mOfferID == 0 && isDeleteOffer())
    {
        if (protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_11))
        {
            setResultMalformed(res);
            return false;
        }
        else if (protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_3))
        {
            setResultNotFound(res);
            return false;
        }
        // Note: This was not invalid before version 3
    }

    if (protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_15) &&
        mOfferID < 0)
    {
        setResultMalformed(res);
        return false;
    }

    return true;
}

void
ManageOfferOpFrameBase::insertLedgerKeysToPrefetch(
    UnorderedSet<LedgerKey>& keys) const
{
    // Prefetch existing offer
    if (mOfferID)
    {
        keys.emplace(offerKey(getSourceID(), mOfferID));
    }

    auto addIssuerAndTrustline = [&](Asset const& asset) {
        if (asset.type() != ASSET_TYPE_NATIVE)
        {
            keys.emplace(trustlineKey(this->getSourceID(), asset));
        }
    };

    addIssuerAndTrustline(mSheep);
    addIssuerAndTrustline(mWheat);
}
}
