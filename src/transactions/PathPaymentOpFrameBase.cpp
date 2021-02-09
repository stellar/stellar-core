// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/PathPaymentOpFrameBase.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/TrustLineWrapper.h"
#include "transactions/TransactionUtils.h"
#include "util/XDROperators.h"

namespace stellar
{

PathPaymentOpFrameBase::PathPaymentOpFrameBase(Operation const& op,
                                               OperationResult& res,
                                               TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
{
}

AccountID
PathPaymentOpFrameBase::getDestID() const
{
    return toAccountID(getDestMuxedAccount());
}

void
PathPaymentOpFrameBase::insertLedgerKeysToPrefetch(
    UnorderedSet<LedgerKey>& keys) const
{
    auto destID = getDestID();
    keys.emplace(accountKey(destID));

    if (getDestAsset().type() != ASSET_TYPE_NATIVE)
    {
        keys.emplace(trustlineKey(destID, getDestAsset()));
    }
    if (getSourceAsset().type() != ASSET_TYPE_NATIVE)
    {
        keys.emplace(trustlineKey(getSourceID(), getSourceAsset()));
    }
}

bool
PathPaymentOpFrameBase::checkIssuer(AbstractLedgerTxn& ltx, Asset const& asset)
{
    if (asset.type() != ASSET_TYPE_NATIVE)
    {
        uint32_t ledgerVersion = ltx.loadHeader().current().ledgerVersion;
        if (ledgerVersion < 13 &&
            !stellar::loadAccountWithoutRecord(ltx, getIssuer(asset)))
        {
            setResultNoIssuer(asset);
            return false;
        }
    }
    return true;
}

bool
PathPaymentOpFrameBase::convert(AbstractLedgerTxn& ltx,
                                int64_t maxOffersToCross,
                                Asset const& sendAsset, int64_t maxSend,
                                int64_t& amountSend, Asset const& recvAsset,
                                int64_t maxRecv, int64_t& amountRecv,
                                RoundingType round,
                                std::vector<ClaimOfferAtom>& offerTrail)
{
    assert(offerTrail.empty());
    assert(!(sendAsset == recvAsset));

    // sendAsset -> recvAsset
    ConvertResult r = convertWithOffers(
        ltx, sendAsset, maxSend, amountSend, recvAsset, maxRecv, amountRecv,
        round,
        [this](LedgerTxnEntry const& o) {
            auto const& offer = o.current().data.offer();
            if (offer.sellerID == getSourceID())
            {
                // we are crossing our own offer
                setResultOfferCrossSelf();
                return OfferFilterResult::eStop;
            }
            return OfferFilterResult::eKeep;
        },
        offerTrail, maxOffersToCross);

    if (amountSend < 0 || amountRecv < 0)
    {
        throw std::runtime_error("amount transferred out of bounds");
    }

    switch (r)
    {
    case ConvertResult::eFilterStop:
        return false;
    case ConvertResult::eOK:
        if (checkTransfer(maxSend, amountSend, maxRecv, amountRecv))
        {
            break;
        }
    // fall through
    case ConvertResult::ePartial:
        setResultTooFewOffers();
        return false;
    case ConvertResult::eCrossedTooMany:
        mResult.code(opEXCEEDED_WORK_LIMIT);
        return false;
    }

    return true;
}

bool
PathPaymentOpFrameBase::shouldBypassIssuerCheck(
    std::vector<Asset> const& path) const
{
    // if the payment doesn't involve intermediate accounts
    // and the destination is the issuer we don't bother
    // checking if the destination account even exist
    // so that it's always possible to send credits back to its issuer
    return (getDestAsset().type() != ASSET_TYPE_NATIVE) && (path.size() == 0) &&
           (getSourceAsset() == getDestAsset()) &&
           (getIssuer(getDestAsset()) == getDestID());
}

bool
PathPaymentOpFrameBase::updateSourceBalance(AbstractLedgerTxn& ltx,
                                            int64_t amount,
                                            bool bypassIssuerCheck,
                                            bool doesSourceAccountExist)
{
    auto const& asset = getSourceAsset();

    if (asset.type() == ASSET_TYPE_NATIVE)
    {
        auto header = ltx.loadHeader();
        LedgerTxnEntry sourceAccount;
        if (header.current().ledgerVersion > 7)
        {
            sourceAccount = stellar::loadAccount(ltx, getSourceID());
            if (!sourceAccount)
            {
                setResultMalformed();
                return false;
            }
        }
        else
        {
            sourceAccount = loadSourceAccount(ltx, header);
        }

        if (amount > getAvailableBalance(header, sourceAccount))
        { // they don't have enough to send
            setResultUnderfunded();
            return false;
        }

        if (!doesSourceAccountExist)
        {
            throw std::runtime_error("modifying account that does not exist");
        }

        auto ok = addBalance(header, sourceAccount, -amount);
        assert(ok);
    }
    else
    {
        if (!bypassIssuerCheck && !checkIssuer(ltx, asset))
        {
            return false;
        }

        auto sourceLine = loadTrustLine(ltx, getSourceID(), asset);
        if (!sourceLine)
        {
            setResultSourceNoTrust();
            return false;
        }

        if (!sourceLine.isAuthorized())
        {
            setResultSourceNotAuthorized();
            return false;
        }

        if (!sourceLine.addBalance(ltx.loadHeader(), -amount))
        {
            setResultUnderfunded();
            return false;
        }
    }

    return true;
}

bool
PathPaymentOpFrameBase::updateDestBalance(AbstractLedgerTxn& ltx,
                                          int64_t amount,
                                          bool bypassIssuerCheck)
{
    auto destID = getDestID();
    auto const& asset = getDestAsset();

    if (asset.type() == ASSET_TYPE_NATIVE)
    {
        auto destination = stellar::loadAccount(ltx, destID);
        if (!addBalance(ltx.loadHeader(), destination, amount))
        {
            if (ltx.loadHeader().current().ledgerVersion >= 11)
            {
                setResultLineFull();
            }
            else
            {
                setResultMalformed();
            }
            return false;
        }
    }
    else
    {
        if (!bypassIssuerCheck && !checkIssuer(ltx, asset))
        {
            return false;
        }

        auto destLine = stellar::loadTrustLine(ltx, destID, asset);
        if (!destLine)
        {
            setResultDestNoTrust();
            return false;
        }

        if (!destLine.isAuthorized())
        {
            setResultDestNotAuthorized();
            return false;
        }

        if (!destLine.addBalance(ltx.loadHeader(), amount))
        {
            setResultLineFull();
            return false;
        }
    }

    return true;
}
}
