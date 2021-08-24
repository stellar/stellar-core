// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/Upgrades.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "database/Database.h"
#include "database/DatabaseUtils.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/TrustLineWrapper.h"
#include "main/Config.h"
#include "transactions/NewSponsorshipUtils.h"
#include "transactions/OfferExchange.h"
#include "transactions/TransactionUtils.h"
#include "util/Decoder.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "util/types.h"
#include <Tracy.hpp>
#include <cereal/archives/json.hpp>
#include <cereal/cereal.hpp>
#include <cereal/types/optional.hpp>
#include <fmt/format.h>
#include <optional>
#include <regex>
#include <xdrpp/marshal.h>

using namespace stellar::SponsorshipUtils;

namespace cereal
{
template <class Archive>
void
save(Archive& ar, stellar::Upgrades::UpgradeParameters const& p)
{
    // NB: See 'rewriteOptionalFieldKeys' below before adding any new fields to
    // this type, and in particular avoid using field names "has" or "val",
    // or serializing any text fields that might have embedded/quoted JSON.
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

std::chrono::hours const Upgrades::UPDGRADE_EXPIRATION_HOURS(12);

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

static std::string
rewriteOptionalFieldKeys(std::string s)
{
    // When transitioning from C++14 to C++17, we migrated from a custom
    // implementation of 'optional' types, to using std::optional.
    //
    // Unfortunately our previous optional-type serialized like this:
    //
    //   {
    //     "has": true,
    //     "val": 12345
    //   }
    //
    // Whereas cereal's built-in support for (de)serializing std::optional will
    // write the same content like this, with the sense of the flag inverted and
    // the names of both fields changed:
    //
    //   {
    //     "nullopt": false,
    //     "data": 12345
    //   }
    //
    // We therefore do a very crude (but safe) string-level rewrite of the
    // former into the latter here. This is safe since none of the fields
    // serialized in the upgrades structure can collide with the string values
    // being substituted, and this is the only structure in the entire program
    // that has this issue.
    //
    // Once this code has been in the field long enough to have processed any
    // pending upgrades deserialized from a database, it should be removed.

    s = std::regex_replace(s, std::regex("\"has\": false"),
                           "\"nullopt\": true");
    s = std::regex_replace(s, std::regex("\"has\": true"),
                           "\"nullopt\": false");
    s = std::regex_replace(s, std::regex("\"val\":"), "\"data\":");
    return s;
}

void
Upgrades::UpgradeParameters::fromJson(std::string const& s)
{
    std::string s1 = rewriteOptionalFieldKeys(s);
    std::istringstream in(s1);
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
Upgrades::applyTo(LedgerUpgrade const& upgrade, AbstractLedgerTxn& ltx)
{
    switch (upgrade.type())
    {
    case LEDGER_UPGRADE_VERSION:
        applyVersionUpgrade(ltx, upgrade.newLedgerVersion());
        break;
    case LEDGER_UPGRADE_BASE_FEE:
        ltx.loadHeader().current().baseFee = upgrade.newBaseFee();
        break;
    case LEDGER_UPGRADE_MAX_TX_SET_SIZE:
        ltx.loadHeader().current().maxTxSetSize = upgrade.newMaxTxSetSize();
        break;
    case LEDGER_UPGRADE_BASE_RESERVE:
        applyReserveUpgrade(ltx, upgrade.newBaseReserve());
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
    std::stringstream r;
    bool first = true;

    auto appendInfo = [&](std::string const& s,
                          std::optional<uint32> const& o) {
        if (o)
        {
            if (first)
            {
                r << fmt::format(
                    "upgradetime={}",
                    VirtualClock::systemPointToISOString(mParams.mUpgradeTime));
                first = false;
            }
            r << fmt::format(", {}={}", s, *o);
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
                         uint64_t closeTime, bool& updated)
{
    updated = false;
    UpgradeParameters res = mParams;

    // If the upgrade time has been surpassed by more than X hours, then remove
    // all upgrades.  This is done so nodes that come up with outdated upgrades
    // don't attempt to change the network
    if (res.mUpgradeTime + Upgrades::UPDGRADE_EXPIRATION_HOURS <=
        VirtualClock::from_time_t(closeTime))
    {
        auto resetParamIfSet = [&](std::optional<uint32>& o) {
            if (o)
            {
                o.reset();
                updated = true;
            }
        };

        resetParamIfSet(res.mProtocolVersion);
        resetParamIfSet(res.mBaseFee);
        resetParamIfSet(res.mMaxTxSize);
        resetParamIfSet(res.mBaseReserve);

        return res;
    }

    auto resetParam = [&](std::optional<uint32>& o, uint32 v) {
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

Upgrades::UpgradeValidity
Upgrades::isValidForApply(UpgradeType const& opaqueUpgrade,
                          LedgerUpgrade& upgrade, LedgerHeader const& header,
                          uint32_t maxLedgerVersion)
{
    try
    {
        xdr::xdr_from_opaque(opaqueUpgrade, upgrade);
    }
    catch (xdr::xdr_runtime_error&)
    {
        return UpgradeValidity::XDR_INVALID;
    }

    bool res = true;
    switch (upgrade.type())
    {
    case LEDGER_UPGRADE_VERSION:
    {
        uint32 newVersion = upgrade.newLedgerVersion();
        // only allow upgrades to a supported version of the protocol
        res = res && (newVersion <= maxLedgerVersion);
        // and enforce versions to be strictly monotonic
        res = res && (newVersion > header.ledgerVersion);
    }
    break;
    case LEDGER_UPGRADE_BASE_FEE:
        res = res && (upgrade.newBaseFee() != 0);
        break;
    case LEDGER_UPGRADE_MAX_TX_SET_SIZE:
        // any size is allowed
        break;
    case LEDGER_UPGRADE_BASE_RESERVE:
        res = res && (upgrade.newBaseReserve() != 0);
        break;
    default:
        res = false;
    }

    return res ? UpgradeValidity::VALID : UpgradeValidity::INVALID;
}

bool
Upgrades::isValidForNomination(LedgerUpgrade const& upgrade,
                               LedgerHeader const& header) const
{
    if (!timeForUpgrade(header.scpValue.closeTime))
    {
        return false;
    }

    switch (upgrade.type())
    {
    case LEDGER_UPGRADE_VERSION:
        return mParams.mProtocolVersion &&
               (upgrade.newLedgerVersion() == *mParams.mProtocolVersion);
    case LEDGER_UPGRADE_BASE_FEE:
        return mParams.mBaseFee && (upgrade.newBaseFee() == *mParams.mBaseFee);
    case LEDGER_UPGRADE_MAX_TX_SET_SIZE:
        return mParams.mMaxTxSize &&
               (upgrade.newMaxTxSetSize() == *mParams.mMaxTxSize);
    case LEDGER_UPGRADE_BASE_RESERVE:
        return mParams.mBaseReserve &&
               (upgrade.newBaseReserve() == *mParams.mBaseReserve);
    default:
        return false;
    }
}

bool
Upgrades::isValid(UpgradeType const& upgrade, LedgerUpgradeType& upgradeType,
                  bool nomination, Config const& cfg,
                  LedgerHeader const& header) const
{
    LedgerUpgrade lupgrade;
    bool res =
        isValidForApply(upgrade, lupgrade, header,
                        cfg.LEDGER_PROTOCOL_VERSION) == UpgradeValidity::VALID;

    if (nomination)
    {
        res = res && isValidForNomination(lupgrade, header);
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
Upgrades::storeUpgradeHistory(Database& db, uint32_t ledgerSeq,
                              LedgerUpgrade const& upgrade,
                              LedgerEntryChanges const& changes, int index)
{
    ZoneScoped;
    xdr::opaque_vec<> upgradeContent(xdr::xdr_to_opaque(upgrade));
    std::string upgradeContent64 = decoder::encode_b64(upgradeContent);

    xdr::opaque_vec<> upgradeChanges(xdr::xdr_to_opaque(changes));
    std::string upgradeChanges64 = decoder::encode_b64(upgradeChanges);

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
    ZoneScoped;
    DatabaseUtils::deleteOldEntriesHelper(db.getSession(), ledgerSeq, count,
                                          "upgradehistory", "ledgerseq");
}

void
Upgrades::deleteNewerEntries(Database& db, uint32_t ledgerSeq)
{
    ZoneScoped;
    DatabaseUtils::deleteNewerEntriesHelper(db.getSession(), ledgerSeq,
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
getAvailableBalanceExcludingLiabilities(AccountID const& accountID,
                                        Asset const& asset,
                                        int64_t balanceAboveReserve,
                                        AbstractLedgerTxn& ltx)
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
        auto trust = stellar::loadTrustLineWithoutRecord(ltx, accountID, asset);
        if (trust && trust.isAuthorizedToMaintainLiabilities())
        {
            return trust.getBalance();
        }
        else
        {
            return 0;
        }
    }
}

static int64_t
getAvailableLimitExcludingLiabilities(AccountID const& accountID,
                                      Asset const& asset, int64_t balance,
                                      AbstractLedgerTxn& ltx)
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
        LedgerKey key(TRUSTLINE);
        key.trustLine().accountID = accountID;
        key.trustLine().asset = assetToTrustLineAsset(asset);
        auto trust = ltx.loadWithoutRecord(key);
        if (trust && isAuthorizedToMaintainLiabilities(trust))
        {
            auto const& tl = trust.current().data.trustLine();
            return tl.limit - tl.balance;
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

enum class UpdateOfferResult
{
    Unchanged,
    Adjusted,
    AdjustedToZero,
    Erase
};

static UpdateOfferResult
updateOffer(
    LedgerTxnEntry& offerEntry, int64_t balance, int64_t balanceAboveReserve,
    std::map<Asset, Liabilities>& liabilities,
    std::map<Asset, std::unique_ptr<int64_t>> const& initialBuyingLiabilities,
    std::map<Asset, std::unique_ptr<int64_t>> const& initialSellingLiabilities,
    AbstractLedgerTxn& ltx, LedgerTxnHeader const& header)
{
    using namespace std::placeholders;
    auto& offer = offerEntry.current().data.offer();

    auto availableBalanceBind =
        std::bind(getAvailableBalanceExcludingLiabilities, offer.sellerID, _1,
                  _2, std::ref(ltx));
    auto availableLimitBind = std::bind(getAvailableLimitExcludingLiabilities,
                                        offer.sellerID, _1, _2, std::ref(ltx));

    bool erase =
        shouldDeleteOffer(offer.selling, balanceAboveReserve,
                          initialSellingLiabilities, availableBalanceBind);
    erase = erase ||
            shouldDeleteOffer(offer.buying, balance, initialBuyingLiabilities,
                              availableLimitBind);
    UpdateOfferResult res =
        erase ? UpdateOfferResult::Erase : UpdateOfferResult::Unchanged;

    // If erase == false then we know that the total buying liabilities
    // of the buying asset do not exceed its available limit, and the
    // total selling liabilities of the selling asset do not exceed its
    // available balance. This implies that there are no excess
    // liabilities for this offer, so the only applicable limit is the
    // offer amount. We then use adjustOffer to check that it will
    // satisfy thresholds.
    if (!erase && adjustOffer(offer.price, offer.amount, INT64_MAX) == 0)
    {
        erase = true;
        res = UpdateOfferResult::AdjustedToZero;
    }

    if (!erase)
    {
        // The same logic for adjustOffer discussed above applies here,
        // except that we now actually update the offer to reflect the
        // adjustment.
        auto adjAmount = adjustOffer(offer.price, offer.amount, INT64_MAX);
        if (adjAmount != offer.amount)
        {
            offer.amount = adjAmount;
            res = UpdateOfferResult::Adjusted;
        }

        if (offer.buying.type() == ASSET_TYPE_NATIVE ||
            !(offer.sellerID == getIssuer(offer.buying)))
        {
            if (!stellar::addBalance(
                    liabilities[offer.buying].buying,
                    getOfferBuyingLiabilities(header, offerEntry)))
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
                    getOfferSellingLiabilities(header, offerEntry)))
            {
                throw std::runtime_error("could not add selling "
                                         "liabilities");
            }
        }
    }
    return res;
}

static UnorderedMap<AccountID, int64_t>
getOfferAccountMinBalances(
    AbstractLedgerTxn& ltx, LedgerTxnHeader const& header,
    std::map<AccountID, std::vector<LedgerTxnEntry>> const& offersByAccount)
{
    UnorderedMap<AccountID, int64_t> minBalanceMap;
    for (auto const& accountOffers : offersByAccount)
    {
        auto const& accountID = accountOffers.first;
        auto accountEntry = stellar::loadAccount(ltx, accountID);
        if (!accountEntry)
        {
            throw std::runtime_error("account does not exist");
        }
        auto const& acc = accountEntry.current().data.account();
        auto minBalance = getMinBalance(header.current(), acc);

        minBalanceMap.emplace(accountID, minBalance);
    }

    return minBalanceMap;
}

static void
eraseOfferWithPossibleSponsorship(AbstractLedgerTxn& ltx,
                                  LedgerTxnEntry& offerEntry,
                                  UnorderedSet<AccountID>& changedAccounts)
{
    LedgerEntry::_ext_t extension = offerEntry.current().ext;
    bool isSponsored = extension.v() == 1 && extension.v1().sponsoringID;
    if (isSponsored)
    {
        // sponsoring account will change
        auto const& sponsoringAcc = *extension.v1().sponsoringID;
        changedAccounts.emplace(sponsoringAcc);
    }

    // This function can't throw here because -
    // If offer is sponsored -
    // 1. ledger version >= 14 is guaranteed
    // 2. numSponsoring for the sponsoring account will be at
    //    least 1 because it's sponsoring this offer
    // 3. The sponsored account will have 1 subEntry and 1
    //    numSponsored because of this offer

    // If offer is not sponsored -
    // 1. the offers account will have at least 1 subEntry
    OwnedEntrySponsorable oes(LedgerEntryKey(offerEntry.current()));
    oes.erase(ltx);
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
prepareLiabilities(AbstractLedgerTxn& ltxOuter)
{
    CLOG_INFO(Ledger, "Starting prepareLiabilities");
    LedgerTxn ltx(ltxOuter);

    auto offersByAccount = ltx.loadAllOffers();
    std::map<AccountID, std::vector<LedgerKey>> keysByAccount;
    for (auto const& kv : offersByAccount)
    {
        std::transform(
            kv.second.begin(), kv.second.end(),
            std::back_inserter(keysByAccount[kv.first]),
            [](auto const& ltxe) { return LedgerEntryKey(ltxe.current()); });
    }

    UnorderedSet<AccountID> changedAccounts;
    uint64_t nChangedTrustLines = 0;

    std::map<UpdateOfferResult, uint64_t> nUpdatedOffers;

    // We want to keep track of the original minBalances before they are changed
    // due to sponsorship changes from deleted offers
    auto offerAccountMinBalanceMap =
        getOfferAccountMinBalances(ltx, ltx.loadHeader(), offersByAccount);
    offersByAccount.clear();

    for (auto& accountKeys : keysByAccount)
    {
        // The purpose of std::unique_ptr here is to have a special value
        // (nullptr) to indicate that an integer overflow would have occurred.
        // Overflow is possible here because existing offers were not
        // constrainted to have int64_t liabilities. This must be carefully
        // handled in what follows.
        std::map<Asset, std::unique_ptr<int64_t>> initialBuyingLiabilities;
        std::map<Asset, std::unique_ptr<int64_t>> initialSellingLiabilities;
        for (auto const& key : accountKeys.second)
        {
            auto offerEntry = ltx.load(key);
            auto const& offer = offerEntry.current().data.offer();
            addLiabilities(
                initialBuyingLiabilities, offer.sellerID, offer.buying,
                getOfferBuyingLiabilities(ltx.loadHeader(), offerEntry));
            addLiabilities(
                initialSellingLiabilities, offer.sellerID, offer.selling,
                getOfferSellingLiabilities(ltx.loadHeader(), offerEntry));
        }

        auto const accountBefore =
            loadAccount(ltx, accountKeys.first).current().data.account();

        // balanceAboveReserve must exclude native selling liabilities, since
        // these are in the process of being recalculated from scratch.
        int64_t balance = accountBefore.balance;

        auto it = offerAccountMinBalanceMap.find(accountKeys.first);
        if (it == offerAccountMinBalanceMap.end())
        {
            // this shouldn't happen. getOfferAccountMinBalances added all keys
            // from offersByAccount
            throw std::runtime_error("min balance missing from map");
        }

        int64_t minBalance = it->second;
        int64_t balanceAboveReserve = balance - minBalance;

        std::map<Asset, Liabilities> liabilities;
        for (auto& key : accountKeys.second)
        {
            auto offerEntry = ltx.load(key);
            auto offerID = offerEntry.current().data.offer().offerID;
            auto res =
                updateOffer(offerEntry, balance, balanceAboveReserve,
                            liabilities, initialBuyingLiabilities,
                            initialSellingLiabilities, ltx, ltx.loadHeader());
            if (res == UpdateOfferResult::AdjustedToZero ||
                res == UpdateOfferResult::Erase)
            {
                eraseOfferWithPossibleSponsorship(ltx, offerEntry,
                                                  changedAccounts);
            }

            ++nUpdatedOffers[res];
            if (res != UpdateOfferResult::Unchanged)
            {
                std::string message;
                switch (res)
                {
                case UpdateOfferResult::Adjusted:
                    message = " was adjusted";
                    break;
                case UpdateOfferResult::AdjustedToZero:
                    message = " was adjusted to zero";
                    break;
                case UpdateOfferResult::Erase:
                    message = " was erased";
                    break;
                default:
                    throw std::runtime_error("Unknown UpdateOfferResult");
                }
                CLOG_DEBUG(Ledger, "Offer with offerID={}{}", offerID, message);
            }
        }

        auto header = ltx.loadHeader();
        auto accountEntry = loadAccount(ltx, accountKeys.first);
        for (auto const& assetLiabilities : liabilities)
        {
            Asset const& asset = assetLiabilities.first;
            Liabilities const& liab = assetLiabilities.second;
            if (asset.type() == ASSET_TYPE_NATIVE)
            {
                int64_t deltaSelling =
                    liab.selling - getSellingLiabilities(header, accountEntry);
                int64_t deltaBuying =
                    liab.buying - getBuyingLiabilities(header, accountEntry);
                if (!addSellingLiabilities(header, accountEntry, deltaSelling))
                {
                    throw std::runtime_error("invalid selling liabilities "
                                             "during upgrade");
                }
                if (!addBuyingLiabilities(header, accountEntry, deltaBuying))
                {
                    throw std::runtime_error("invalid buying liabilities "
                                             "during upgrade");
                }
            }
            else
            {
                auto trustEntry =
                    stellar::loadTrustLine(ltx, accountKeys.first, asset);
                int64_t deltaSelling =
                    liab.selling - trustEntry.getSellingLiabilities(header);
                int64_t deltaBuying =
                    liab.buying - trustEntry.getBuyingLiabilities(header);
                if (deltaSelling != 0 || deltaBuying != 0)
                {
                    ++nChangedTrustLines;
                }

                // the deltas should only be positive when liabilities were
                // introduced in ledgerVersion 10
                if (header.current().ledgerVersion > 10 &&
                    (deltaSelling > 0 || deltaBuying > 0))
                {
                    throw std::runtime_error("invalid liabilities delta");
                }

                if (!trustEntry.addSellingLiabilities(header, deltaSelling))
                {
                    throw std::runtime_error("invalid selling liabilities "
                                             "during upgrade");
                }
                if (!trustEntry.addBuyingLiabilities(header, deltaBuying))
                {
                    throw std::runtime_error("invalid buying liabilities "
                                             "during upgrade");
                }
            }
        }

        if (!(accountEntry.current().data.account() == accountBefore))
        {
            changedAccounts.emplace(
                accountEntry.current().data.account().accountID);
        }
    }

    ltx.commit();
    CLOG_INFO(Ledger,
              "prepareLiabilities completed with {} accounts modified, {} "
              "trustlines modified, {} offers adjusted, {} offers adjusted to "
              "zero, and {} offers erased",
              changedAccounts.size(), nChangedTrustLines,
              nUpdatedOffers[UpdateOfferResult::Adjusted],
              nUpdatedOffers[UpdateOfferResult::AdjustedToZero],
              nUpdatedOffers[UpdateOfferResult::Erase]);
}

static void
upgradeFromProtocol15To16(AbstractLedgerTxn& ltx)
{
    if (gIsProductionNetwork)
    {
        auto const sellerStrKey =
            "GBGP52VDS2U3F4VZHEMD4MDDM7YIODXLYVGOZLYSTAD6PZK45JXILTAX";
        auto const offerID = 289733046;

        auto const sellerID = KeyUtils::fromStrKey<PublicKey>(sellerStrKey);
        auto seller = stellar::loadAccountWithoutRecord(ltx, sellerID);
        auto offer = stellar::loadOffer(ltx, sellerID, offerID);

        if (offer)
        {
            // Seller exists if offer exists
            auto const& ae = seller.current().data.account();
            if (ae.ext.v() == 1 && ae.ext.v1().ext.v() == 2)
            {
                CLOG_ERROR(Ledger,
                           "Account {} has AccountEntryExtensionV2, cannot "
                           "complete upgrade",
                           sellerStrKey);
                return;
            }

            auto const sponsorStrKey =
                "GAS3CQSW3HE27IF5KDWKCM7K6FG6AHRHWOUVBUWIRV4ZGTJMPBXNGATF";
            auto const sponsorID =
                KeyUtils::fromStrKey<PublicKey>(sponsorStrKey);
            auto const& le = offer.current();
            if (le.ext.v() == 1 && le.ext.v1().sponsoringID &&
                *le.ext.v1().sponsoringID == sponsorID)
            {
                offer.current().ext.v(0);
                CLOG_ERROR(Ledger, "Sponsorship removed from offer {}",
                           offerID);
                return;
            }
        }

        CLOG_ERROR(Ledger, "Offer {} does not exist", offerID);
    }
}

void
Upgrades::applyVersionUpgrade(AbstractLedgerTxn& ltx, uint32_t newVersion)
{
    auto header = ltx.loadHeader();
    uint32_t prevVersion = header.current().ledgerVersion;

    header.current().ledgerVersion = newVersion;
    if (header.current().ledgerVersion >= 10 && prevVersion < 10)
    {
        prepareLiabilities(ltx);
    }
    else if (header.current().ledgerVersion == 16 && prevVersion == 15)
    {
        upgradeFromProtocol15To16(ltx);
    }
}

void
Upgrades::applyReserveUpgrade(AbstractLedgerTxn& ltx, uint32_t newReserve)
{
    auto header = ltx.loadHeader();
    bool didReserveIncrease = newReserve > header.current().baseReserve;

    header.current().baseReserve = newReserve;
    if (header.current().ledgerVersion >= 10 && didReserveIncrease)
    {
        prepareLiabilities(ltx);
    }
}
}
