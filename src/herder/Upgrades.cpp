// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/Upgrades.h"
#include "database/Database.h"
#include "database/DatabaseUtils.h"
#include "ledger/AccountFrame.h"
#include "ledger/LedgerDelta.h"
#include "ledger/LedgerManager.h"
#include "ledger/OfferFrame.h"
#include "ledger/TrustFrame.h"
#include "main/Config.h"
#include "transactions/OfferExchange.h"
#include "util/Decoder.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "util/types.h"
#include <cereal/archives/json.hpp>
#include <cereal/cereal.hpp>
#include <lib/util/format.h>
#include <xdrpp/marshal.h>

namespace cereal
{
template <class Archive>
void
save(Archive& ar, stellar::Upgrades::UpgradeParameters const& p)
{
    ar(make_nvp("time", stellar::VirtualClock::to_time_t(p.mUpgradeTime)));
    ar(make_nvp("version", p.mProtocolVersion));
    ar(make_nvp("fee", p.mBaseFee));
    ar(make_nvp("maxtxsize", p.mMaxTxSize));
    ar(make_nvp("reserve", p.mBaseReserve));
}

template <class Archive>
void
load(Archive& ar, stellar::Upgrades::UpgradeParameters& o)
{
    time_t t;
    ar(make_nvp("time", t));
    o.mUpgradeTime = stellar::VirtualClock::from_time_t(t);
    ar(make_nvp("version", o.mProtocolVersion));
    ar(make_nvp("fee", o.mBaseFee));
    ar(make_nvp("maxtxsize", o.mMaxTxSize));
    ar(make_nvp("reserve", o.mBaseReserve));
}
} // namespace cereal

namespace stellar
{
std::string
Upgrades::UpgradeParameters::toJson() const
{
    std::ostringstream out;
    {
        cereal::JSONOutputArchive ar(out);
        cereal::save(ar, *this);
    }
    return out.str();
}

void
Upgrades::UpgradeParameters::fromJson(std::string const& s)
{
    std::istringstream in(s);
    {
        cereal::JSONInputArchive ar(in);
        cereal::load(ar, *this);
    }
}

Upgrades::Upgrades(UpgradeParameters const& params) : mParams(params)
{
}

void
Upgrades::setParameters(UpgradeParameters const& params, Config const& cfg)
{
    if (params.mProtocolVersion &&
        *params.mProtocolVersion > cfg.LEDGER_PROTOCOL_VERSION)
    {
        throw std::invalid_argument(fmt::format(
            "Protocol version error: supported is up to {}, passed is {}",
            cfg.LEDGER_PROTOCOL_VERSION, *params.mProtocolVersion));
    }
    mParams = params;
}

Upgrades::UpgradeParameters const&
Upgrades::getParameters() const
{
    return mParams;
}

std::vector<LedgerUpgrade>
Upgrades::createUpgradesFor(LedgerHeader const& header) const
{
    auto result = std::vector<LedgerUpgrade>{};
    if (!timeForUpgrade(header.scpValue.closeTime))
    {
        return result;
    }

    if (mParams.mProtocolVersion &&
        (header.ledgerVersion != *mParams.mProtocolVersion))
    {
        result.emplace_back(LEDGER_UPGRADE_VERSION);
        result.back().newLedgerVersion() = *mParams.mProtocolVersion;
    }
    if (mParams.mBaseFee && (header.baseFee != *mParams.mBaseFee))
    {
        result.emplace_back(LEDGER_UPGRADE_BASE_FEE);
        result.back().newBaseFee() = *mParams.mBaseFee;
    }
    if (mParams.mMaxTxSize && (header.maxTxSetSize != *mParams.mMaxTxSize))
    {
        result.emplace_back(LEDGER_UPGRADE_MAX_TX_SET_SIZE);
        result.back().newMaxTxSetSize() = *mParams.mMaxTxSize;
    }
    if (mParams.mBaseReserve && (header.baseReserve != *mParams.mBaseReserve))
    {
        result.emplace_back(LEDGER_UPGRADE_BASE_RESERVE);
        result.back().newBaseReserve() = *mParams.mBaseReserve;
    }

    return result;
}

void
Upgrades::applyTo(LedgerUpgrade const& upgrade, LedgerManager& ledgerManager,
                  LedgerDelta& ld)
{
    LedgerHeader& header = ld.getHeader();
    switch (upgrade.type())
    {
    case LEDGER_UPGRADE_VERSION:
        applyVersionUpgrade(ledgerManager, ld, upgrade.newLedgerVersion());
        break;
    case LEDGER_UPGRADE_BASE_FEE:
        header.baseFee = upgrade.newBaseFee();
        break;
    case LEDGER_UPGRADE_MAX_TX_SET_SIZE:
        header.maxTxSetSize = upgrade.newMaxTxSetSize();
        break;
    case LEDGER_UPGRADE_BASE_RESERVE:
        applyReserveUpgrade(ledgerManager, ld, upgrade.newBaseReserve());
        break;
    default:
    {
        auto s = fmt::format("Unknown upgrade type: {0}", upgrade.type());
        throw std::runtime_error(s);
    }
    }
}

std::string
Upgrades::toString(LedgerUpgrade const& upgrade)
{
    switch (upgrade.type())
    {
    case LEDGER_UPGRADE_VERSION:
        return fmt::format("protocolversion={0}", upgrade.newLedgerVersion());
    case LEDGER_UPGRADE_BASE_FEE:
        return fmt::format("basefee={0}", upgrade.newBaseFee());
    case LEDGER_UPGRADE_MAX_TX_SET_SIZE:
        return fmt::format("maxtxsetsize={0}", upgrade.newMaxTxSetSize());
    case LEDGER_UPGRADE_BASE_RESERVE:
        return fmt::format("basereserve={0}", upgrade.newBaseReserve());
    default:
        return "<unsupported>";
    }
}

std::string
Upgrades::toString() const
{
    fmt::MemoryWriter r;

    auto appendInfo = [&](std::string const& s, optional<uint32> const& o) {
        if (o)
        {
            if (!r.size())
            {
                r << "upgradetime="
                  << VirtualClock::pointToISOString(mParams.mUpgradeTime);
            }
            r << ", " << s << "=" << *o;
        }
    };
    appendInfo("protocolversion", mParams.mProtocolVersion);
    appendInfo("basefee", mParams.mBaseFee);
    appendInfo("basereserve", mParams.mBaseReserve);
    appendInfo("maxtxsize", mParams.mMaxTxSize);

    return r.str();
}

Upgrades::UpgradeParameters
Upgrades::removeUpgrades(std::vector<UpgradeType>::const_iterator beginUpdates,
                         std::vector<UpgradeType>::const_iterator endUpdates,
                         bool& updated)
{
    updated = false;
    UpgradeParameters res = mParams;

    auto resetParam = [&](optional<uint32>& o, uint32 v) {
        if (o && *o == v)
        {
            o.reset();
            updated = true;
        }
    };

    for (auto it = beginUpdates; it != endUpdates; it++)
    {
        auto& u = *it;
        LedgerUpgrade lu;
        try
        {
            xdr::xdr_from_opaque(u, lu);
        }
        catch (xdr::xdr_runtime_error&)
        {
            continue;
        }
        switch (lu.type())
        {
        case LEDGER_UPGRADE_VERSION:
            resetParam(res.mProtocolVersion, lu.newLedgerVersion());
            break;
        case LEDGER_UPGRADE_BASE_FEE:
            resetParam(res.mBaseFee, lu.newBaseFee());
            break;
        case LEDGER_UPGRADE_MAX_TX_SET_SIZE:
            resetParam(res.mMaxTxSize, lu.newMaxTxSetSize());
            break;
        case LEDGER_UPGRADE_BASE_RESERVE:
            resetParam(res.mBaseReserve, lu.newBaseReserve());
            break;
        default:
            // skip unknown
            break;
        }
    }
    return res;
}

bool
Upgrades::isValid(UpgradeType const& upgrade, LedgerUpgradeType& upgradeType,
                  bool nomination, Config const& cfg,
                  LedgerHeader const& header) const
{
    if (nomination && !timeForUpgrade(header.scpValue.closeTime))
    {
        return false;
    }

    LedgerUpgrade lupgrade;

    try
    {
        xdr::xdr_from_opaque(upgrade, lupgrade);
    }
    catch (xdr::xdr_runtime_error&)
    {
        return false;
    }

    bool res = true;
    switch (lupgrade.type())
    {
    case LEDGER_UPGRADE_VERSION:
    {
        uint32 newVersion = lupgrade.newLedgerVersion();
        if (nomination)
        {
            res = mParams.mProtocolVersion &&
                  (newVersion == *mParams.mProtocolVersion);
        }
        // only allow upgrades to a supported version of the protocol
        res = res && (newVersion <= cfg.LEDGER_PROTOCOL_VERSION);
        // and enforce versions to be strictly monotonic
        res = res && (newVersion > header.ledgerVersion);
    }
    break;
    case LEDGER_UPGRADE_BASE_FEE:
    {
        uint32 newFee = lupgrade.newBaseFee();
        if (nomination)
        {
            res = mParams.mBaseFee && (newFee == *mParams.mBaseFee);
        }
        res = res && (newFee != 0);
    }
    break;
    case LEDGER_UPGRADE_MAX_TX_SET_SIZE:
    {
        uint32 newMax = lupgrade.newMaxTxSetSize();
        if (nomination)
        {
            res = mParams.mMaxTxSize && (newMax == *mParams.mMaxTxSize);
        }
        res = res && (newMax != 0);
    }
    break;
    case LEDGER_UPGRADE_BASE_RESERVE:
    {
        uint32 newReserve = lupgrade.newBaseReserve();
        if (nomination)
        {
            res = mParams.mBaseReserve && (newReserve == *mParams.mBaseReserve);
        }
        res = res && (newReserve != 0);
    }
    break;
    default:
        res = false;
    }
    if (res)
    {
        upgradeType = lupgrade.type();
    }
    return res;
}

bool
Upgrades::timeForUpgrade(uint64_t time) const
{
    return mParams.mUpgradeTime <= VirtualClock::from_time_t(time);
}

void
Upgrades::dropAll(Database& db)
{
    db.getSession() << "DROP TABLE IF EXISTS upgradehistory";
    db.getSession() << "CREATE TABLE upgradehistory ("
                       "ledgerseq    INT NOT NULL CHECK (ledgerseq >= 0), "
                       "upgradeindex INT NOT NULL, "
                       "upgrade      TEXT NOT NULL, "
                       "changes      TEXT NOT NULL, "
                       "PRIMARY KEY (ledgerseq, upgradeindex)"
                       ")";
    db.getSession()
        << "CREATE INDEX upgradehistbyseq ON upgradehistory (ledgerseq);";
}

void
Upgrades::storeUpgradeHistory(LedgerManager& ledgerManager,
                              LedgerUpgrade const& upgrade,
                              LedgerEntryChanges const& changes, int index)
{
    uint32_t ledgerSeq = ledgerManager.getCurrentLedgerHeader().ledgerSeq;

    xdr::opaque_vec<> upgradeContent(xdr::xdr_to_opaque(upgrade));
    std::string upgradeContent64 = decoder::encode_b64(upgradeContent);

    xdr::opaque_vec<> upgradeChanges(xdr::xdr_to_opaque(changes));
    std::string upgradeChanges64 = decoder::encode_b64(upgradeChanges);

    auto& db = ledgerManager.getDatabase();
    auto prep = db.getPreparedStatement(
        "INSERT INTO upgradehistory "
        "(ledgerseq, upgradeindex,  upgrade,  changes) VALUES "
        "(:seq,      :upgradeindex, :upgrade, :changes)");

    auto& st = prep.statement();
    st.exchange(soci::use(ledgerSeq));
    st.exchange(soci::use(index));
    st.exchange(soci::use(upgradeContent64));
    st.exchange(soci::use(upgradeChanges64));
    st.define_and_bind();
    {
        auto timer = db.getInsertTimer("upgradehistory");
        st.execute(true);
    }

    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("Could not update data in SQL");
    }
}

void
Upgrades::deleteOldEntries(Database& db, uint32_t ledgerSeq, uint32_t count)
{
    DatabaseUtils::deleteOldEntriesHelper(db.getSession(), ledgerSeq, count,
                                          "upgradehistory", "ledgerseq");
}

static void
addLiabilities(std::map<Asset, std::unique_ptr<int64_t>>& liabilities,
               AccountID const& accountID, Asset const& asset, int64_t delta)
{
    auto iter =
        liabilities.insert(std::make_pair(asset, std::make_unique<int64_t>(0)))
            .first;
    if (asset.type() != ASSET_TYPE_NATIVE && accountID == getIssuer(asset))
    {
        return;
    }
    if (iter->second)
    {
        if (!stellar::addBalance(*iter->second, delta))
        {
            iter->second.reset();
        }
    }
}

static int64_t
getAvailableBalance(AccountID const& accountID, Asset const& asset,
                    int64_t balanceAboveReserve, LedgerDelta& ld, Database& db)
{
    if (asset.type() == ASSET_TYPE_NATIVE)
    {
        return balanceAboveReserve;
    }

    if (accountID == getIssuer(asset))
    {
        return INT64_MAX;
    }
    else
    {
        auto trust = TrustFrame::loadTrustLine(accountID, asset, db, &ld);
        if (trust && trust->isAuthorized())
        {
            return trust->getBalance();
        }
        else
        {
            return 0;
        }
    }
}

static int64_t
getAvailableLimit(AccountID const& accountID, Asset const& asset,
                  int64_t balance, LedgerDelta& ld, Database& db)
{
    if (asset.type() == ASSET_TYPE_NATIVE)
    {
        return INT64_MAX - balance;
    }

    if (accountID == getIssuer(asset))
    {
        return INT64_MAX;
    }
    else
    {
        auto trust = TrustFrame::loadTrustLine(accountID, asset, db, &ld);
        if (trust && trust->isAuthorized())
        {
            return trust->getTrustLine().limit - trust->getBalance();
        }
        else
        {
            return 0;
        }
    }
}

static bool
shouldDeleteOffer(Asset const& asset, int64_t effectiveBalance,
                  std::map<Asset, std::unique_ptr<int64_t>> const& liabilities,
                  std::function<int64_t(Asset const&, int64_t)> getCap)
{
    auto iter = liabilities.find(asset);
    if (iter == liabilities.end())
    {
        throw std::runtime_error("liabilities were not calculated");
    }
    // Offers should be deleted if liabilities exceed INT64_MAX (nullptr) or if
    // there are excess liabilities.
    return iter->second ? *iter->second > getCap(asset, effectiveBalance)
                        : true;
}

// This function is used to bring offers and liabilities into a valid state.
// For every account that has offers,
//   1. Calculate total liabilities for each asset
//   2. For every asset with excess buying liabilities according to (1), erase
//      all offers buying that asset. For every asset with excess selling
//      liabilities according to (1), erase all offers selling that asset.
//   3. Update liabilities to reflect offers remaining in the book.
// It is essential to note that the excess liabilities are determined only
// using the initial result of step (1), so it does not matter what order the
// offers are processed.
static void
prepareLiabilities(LedgerManager& ledgerManager, LedgerDelta& ld)
{
    using namespace std::placeholders;

    auto& db = ledgerManager.getDatabase();
    db.getEntryCache().clear();
    auto offersByAccount = OfferFrame::loadAllOffers(db);

    for (auto& accountOffers : offersByAccount)
    {
        // The purpose of std::unique_ptr here is to have a special value
        // (nullptr) to indicate that an integer overflow would have occured.
        // Overflow is possible here because existing offers were not
        // constrainted to have int64_t liabilities. This must be carefully
        // handled in what follows.
        std::map<Asset, std::unique_ptr<int64_t>> initialBuyingLiabilities;
        std::map<Asset, std::unique_ptr<int64_t>> initialSellingLiabilities;
        for (auto const& offerFrame : accountOffers.second)
        {
            auto const& offer = offerFrame->getOffer();
            addLiabilities(initialBuyingLiabilities, offer.sellerID,
                           offer.buying, offerFrame->getBuyingLiabilities());
            addLiabilities(initialSellingLiabilities, offer.sellerID,
                           offer.selling, offerFrame->getSellingLiabilities());
        }

        auto accountFrame =
            AccountFrame::loadAccount(ld, accountOffers.first, db);
        if (!accountFrame)
        {
            throw std::runtime_error("account does not exist");
        }

        // balanceAboveReserve must exclude native selling liabilities, since
        // these are in the process of being recalculated from scratch.
        int64_t balance = accountFrame->getBalance();
        int64_t minBalance = accountFrame->getMinimumBalance(ledgerManager);
        int64_t balanceAboveReserve = balance - minBalance;

        std::map<Asset, Liabilities> liabilities;
        for (auto const& offerFrame : accountOffers.second)
        {
            auto& offer = offerFrame->getOffer();

            auto availableBalanceBind =
                std::bind(getAvailableBalance, offer.sellerID, _1, _2,
                          std::ref(ld), std::ref(db));
            auto availableLimitBind =
                std::bind(getAvailableLimit, offer.sellerID, _1, _2,
                          std::ref(ld), std::ref(db));

            bool erase = shouldDeleteOffer(offer.selling, balanceAboveReserve,
                                           initialSellingLiabilities,
                                           availableBalanceBind);
            erase = erase || shouldDeleteOffer(offer.buying, balance,
                                               initialBuyingLiabilities,
                                               availableLimitBind);

            // If erase == false then we know that the total buying liabilities
            // of the buying asset do not exceed its available limit, and the
            // total selling liabilities of the selling asset do not exceed its
            // available balance. This implies that there are no excess
            // liabilities for this offer, so the only applicable limit is the
            // offer amount. We then use adjustOffer to check that it will
            // satisfy thresholds.
            erase =
                erase || (adjustOffer(offerFrame->getPrice(),
                                      offerFrame->getAmount(), INT64_MAX) == 0);

            if (erase)
            {
                accountFrame->addNumEntries(-1, ledgerManager);
                offerFrame->storeDelete(ld, db);
            }
            else
            {
                // The same logic for adjustOffer discussed above applies here,
                // except that we now actually update the offer to reflect the
                // adjustment.
                offer.amount = adjustOffer(offerFrame->getPrice(),
                                           offerFrame->getAmount(), INT64_MAX);
                offerFrame->storeChange(ld, db);

                if (offer.buying.type() == ASSET_TYPE_NATIVE ||
                    !(offer.sellerID == getIssuer(offer.buying)))
                {
                    if (!stellar::addBalance(
                            liabilities[offer.buying].buying,
                            offerFrame->getBuyingLiabilities()))
                    {
                        throw std::runtime_error("could not add buying "
                                                 "liabilities");
                    }
                }
                if (offer.selling.type() == ASSET_TYPE_NATIVE ||
                    !(offer.sellerID == getIssuer(offer.selling)))
                {
                    if (!stellar::addBalance(
                            liabilities[offer.selling].selling,
                            offerFrame->getSellingLiabilities()))
                    {
                        throw std::runtime_error("could not add selling "
                                                 "liabilities");
                    }
                }
            }
        }

        for (auto const& assetLiabilities : liabilities)
        {
            Asset const& asset = assetLiabilities.first;
            Liabilities const& liab = assetLiabilities.second;
            if (asset.type() == ASSET_TYPE_NATIVE)
            {
                int64_t deltaSelling =
                    liab.selling -
                    accountFrame->getSellingLiabilities(ledgerManager);
                int64_t deltaBuying =
                    liab.buying -
                    accountFrame->getBuyingLiabilities(ledgerManager);
                if (!accountFrame->addSellingLiabilities(deltaSelling,
                                                         ledgerManager))
                {
                    throw std::runtime_error("invalid selling liabilities "
                                             "during upgrade");
                }
                if (!accountFrame->addBuyingLiabilities(deltaBuying,
                                                        ledgerManager))
                {
                    throw std::runtime_error("invalid buying liabilities "
                                             "during upgrade");
                }
            }
            else
            {
                auto trustFrame = TrustFrame::loadTrustLine(accountOffers.first,
                                                            asset, db, &ld);
                int64_t deltaSelling =
                    liab.selling -
                    trustFrame->getSellingLiabilities(ledgerManager);
                int64_t deltaBuying =
                    liab.buying -
                    trustFrame->getBuyingLiabilities(ledgerManager);
                if (!trustFrame->addSellingLiabilities(deltaSelling,
                                                       ledgerManager))
                {
                    throw std::runtime_error("invalid selling liabilities "
                                             "during upgrade");
                }
                if (!trustFrame->addBuyingLiabilities(deltaBuying,
                                                      ledgerManager))
                {
                    throw std::runtime_error("invalid buying liabilities "
                                             "during upgrade");
                }
                trustFrame->storeChange(ld, db);
            }
        }

        accountFrame->storeChange(ld, db);
    }

    db.getEntryCache().clear();
}

void
Upgrades::applyVersionUpgrade(LedgerManager& ledgerManager, LedgerDelta& ld,
                              uint32_t newVersion)
{
    LedgerHeader& header = ld.getHeader();
    uint32_t prevVersion = header.ledgerVersion;

    header.ledgerVersion = newVersion;
    ledgerManager.getCurrentLedgerHeader().ledgerVersion = newVersion;
    if (header.ledgerVersion >= 10 && prevVersion < 10)
    {
        prepareLiabilities(ledgerManager, ld);
    }
}

void
Upgrades::applyReserveUpgrade(LedgerManager& ledgerManager, LedgerDelta& ld,
                              uint32_t newReserve)
{
    LedgerHeader& header = ld.getHeader();
    bool didReserveIncrease = newReserve > header.baseReserve;

    header.baseReserve = newReserve;
    ledgerManager.getCurrentLedgerHeader().baseReserve = newReserve;
    if (header.ledgerVersion >= 10 && didReserveIncrease)
    {
        prepareLiabilities(ledgerManager, ld);
    }
}
}
