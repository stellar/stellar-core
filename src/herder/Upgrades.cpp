// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/Upgrades.h"
#include "bucket/BucketManager.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "database/Database.h"
#include "database/DatabaseUtils.h"
#include "herder/Herder.h"
#include "ledger/LedgerHeaderUtils.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/LedgerTypeUtils.h"
#include "ledger/NetworkConfig.h"
#include "ledger/TrustLineWrapper.h"
#include "main/Config.h"
#include "rust/RustBridge.h"
#include "transactions/OfferExchange.h"
#include "transactions/SponsorshipUtils.h"
#include "transactions/TransactionUtils.h"
#include "util/Decoder.h"
#include "util/Logging.h"
#include "util/ProtocolVersion.h"
#include "util/Timer.h"
#include "util/XDRCereal.h"
#include "util/types.h"
#include "xdrpp/printer.h"
#include <Tracy.hpp>
#include <cereal/archives/json.hpp>
#include <cereal/cereal.hpp>
#include <cereal/types/optional.hpp>
#include <fmt/format.h>
#include <optional>
#include <regex>
#include <xdrpp/marshal.h>

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
    ar(make_nvp("maxtxsize", p.mMaxTxSetSize));
    ar(make_nvp("reserve", p.mBaseReserve));
    ar(make_nvp("flags", p.mFlags));

    std::optional<std::string> configUpgradeKeyStr;
    if (p.mConfigUpgradeSetKey)
    {
        configUpgradeKeyStr = stellar::decoder::encode_b64(
            xdr::xdr_to_opaque(*p.mConfigUpgradeSetKey));
    }
    ar(make_nvp("configupgradesetkey", configUpgradeKeyStr));
    ar(make_nvp("maxsorobantxsetsize", p.mMaxSorobanTxSetSize));
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
    ar(make_nvp("maxtxsize", o.mMaxTxSetSize));
    ar(make_nvp("reserve", o.mBaseReserve));

    // the flags and configupgrade upgrades were added after the fields above,
    // so it's possible for them not to exist in the database
    try
    {
        ar(make_nvp("flags", o.mFlags));
        ar(make_nvp("maxsorobantxsetsize", o.mMaxSorobanTxSetSize));

        std::optional<std::string> configUpgradeKeyStr;
        ar(make_nvp("configupgradesetkey", configUpgradeKeyStr));

        if (configUpgradeKeyStr)
        {
            std::vector<uint8_t> buffer;
            stellar::decoder::decode_b64(configUpgradeKeyStr.value(), buffer);
            stellar::ConfigUpgradeSetKey key;
            xdr::xdr_from_opaque(buffer, key);

            o.mConfigUpgradeSetKey = key;
        }
        else
        {
            o.mConfigUpgradeSetKey.reset();
        }
    }
    catch (cereal::Exception&)
    {
        // flags or configupgrade name not found
    }
    catch (std::exception&)
    {
        // Invalid base64 or xdr for configupgrade
    }
}
} // namespace cereal

namespace stellar
{
namespace
{
uint32_t
readMaxSorobanTxSetSize(LedgerSnapshot const& ls)
{
    LedgerKey key(LedgerEntryType::CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_EXECUTION_LANES;
    return ls.load(key)
        .current()
        .data.configSetting()
        .contractExecutionLanes()
        .ledgerMaxTxCount;
}

void
upgradeMaxSorobanTxSetSize(AbstractLedgerTxn& ltx, uint32_t maxTxSetSize)
{
    LedgerKey key(LedgerEntryType::CONFIG_SETTING);
    key.configSetting().configSettingID =
        ConfigSettingID::CONFIG_SETTING_CONTRACT_EXECUTION_LANES;
    auto& le = ltx.load(key).current();
    le.data.configSetting().contractExecutionLanes().ledgerMaxTxCount =
        maxTxSetSize;
}
} // namespace
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

std::string
Upgrades::UpgradeParameters::toDebugJson(LedgerSnapshot const& ls) const
{
    Json::Value upgradesJson;
    Json::Reader reader;
    reader.parse(toJson(), upgradesJson);

    if (mConfigUpgradeSetKey)
    {
        // combine the key and actual config upgrade set under a single Json
        // value
        upgradesJson["configupgradeinfo"]["configupgradesetkey"] =
            upgradesJson["configupgradesetkey"];
        upgradesJson.removeMember("configupgradesetkey");

        auto upgradeSetPtr =
            ConfigUpgradeSetFrame::makeFromKey(ls, *mConfigUpgradeSetKey);
        if (upgradeSetPtr)
        {
            Json::Value configUpgradeSetJson;
            Json::Reader reader2;
            reader2.parse(upgradeSetPtr->toJson(), configUpgradeSetJson);
            upgradesJson["configupgradeinfo"]["configupgradeset"] =
                configUpgradeSetJson;
        }
    }

    Json::StyledWriter writer;
    return writer.write(upgradesJson);
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
        throw std::invalid_argument(
            fmt::format(FMT_STRING("Protocol version error: supported is up to "
                                   "{:d}, passed is {:d}"),
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
Upgrades::createUpgradesFor(LedgerHeader const& lclHeader,
                            LedgerSnapshot const& ls) const
{
    auto result = std::vector<LedgerUpgrade>{};
    if (!timeForUpgrade(lclHeader.scpValue.closeTime))
    {
        return result;
    }

    if (mParams.mProtocolVersion &&
        (lclHeader.ledgerVersion != *mParams.mProtocolVersion))
    {
        result.emplace_back(LEDGER_UPGRADE_VERSION);
        result.back().newLedgerVersion() = *mParams.mProtocolVersion;
    }
    if (mParams.mBaseFee && (lclHeader.baseFee != *mParams.mBaseFee))
    {
        result.emplace_back(LEDGER_UPGRADE_BASE_FEE);
        result.back().newBaseFee() = *mParams.mBaseFee;
    }
    if (mParams.mMaxTxSetSize &&
        (lclHeader.maxTxSetSize != *mParams.mMaxTxSetSize))
    {
        result.emplace_back(LEDGER_UPGRADE_MAX_TX_SET_SIZE);
        result.back().newMaxTxSetSize() = *mParams.mMaxTxSetSize;
    }
    if (mParams.mBaseReserve &&
        (lclHeader.baseReserve != *mParams.mBaseReserve))
    {
        result.emplace_back(LEDGER_UPGRADE_BASE_RESERVE);
        result.back().newBaseReserve() = *mParams.mBaseReserve;
    }
    if (mParams.mFlags)
    {
        if (LedgerHeaderUtils::getFlags(lclHeader) != *mParams.mFlags)
        {
            result.emplace_back(LEDGER_UPGRADE_FLAGS);
            result.back().newFlags() = *mParams.mFlags;
        }
    }
    if (mParams.mMaxSorobanTxSetSize)
    {
        if (protocolVersionStartsFrom(lclHeader.ledgerVersion,
                                      SOROBAN_PROTOCOL_VERSION) &&
            readMaxSorobanTxSetSize(ls) != *mParams.mMaxSorobanTxSetSize)
        {
            result.emplace_back(LEDGER_UPGRADE_MAX_SOROBAN_TX_SET_SIZE);
            result.back().newMaxSorobanTxSetSize() =
                *mParams.mMaxSorobanTxSetSize;
        }
    }
    auto key = mParams.mConfigUpgradeSetKey;
    if (key)
    {
        auto cfgUpgrade = ConfigUpgradeSetFrame::makeFromKey(ls, *key);
        if (cfgUpgrade != nullptr &&
            cfgUpgrade->isValidForApply() == UpgradeValidity::VALID &&
            cfgUpgrade->upgradeNeeded(ls))
        {
            result.emplace_back(LEDGER_UPGRADE_CONFIG);
            result.back().newConfig() = cfgUpgrade->getKey();
        }
    }
    return result;
}

void
Upgrades::applyTo(LedgerUpgrade const& upgrade, Application& app,
                  AbstractLedgerTxn& ltx)
{
    switch (upgrade.type())
    {
    case LEDGER_UPGRADE_VERSION:
        applyVersionUpgrade(app, ltx, upgrade.newLedgerVersion());
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
    case LEDGER_UPGRADE_FLAGS:
        setLedgerHeaderFlag(ltx.loadHeader().current(), upgrade.newFlags());
        break;
    case LEDGER_UPGRADE_CONFIG:
    {
        LedgerSnapshot ltxState(ltx);
        auto cfgUpgrade =
            ConfigUpgradeSetFrame::makeFromKey(ltxState, upgrade.newConfig());
        if (!cfgUpgrade)
        {
            throw std::runtime_error(
                "Failed to retrieve valid config upgrade set");
        }
        if (cfgUpgrade->isValidForApply() != Upgrades::UpgradeValidity::VALID)
        {
            throw std::runtime_error("config upgrade set is no longer valid");
        }
        CLOG_INFO(Ledger, "Applying config upgrade: {}", cfgUpgrade->toJson());
        cfgUpgrade->applyTo(ltx);
        break;
    }
    case LEDGER_UPGRADE_MAX_SOROBAN_TX_SET_SIZE:
        upgradeMaxSorobanTxSetSize(ltx, upgrade.newMaxSorobanTxSetSize());
        break;
    default:
    {
        auto s =
            fmt::format(FMT_STRING("Unknown upgrade type: {}"), upgrade.type());
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
        return fmt::format(FMT_STRING("protocolversion={:d}"),
                           upgrade.newLedgerVersion());
    case LEDGER_UPGRADE_BASE_FEE:
        return fmt::format(FMT_STRING("basefee={:d}"), upgrade.newBaseFee());
    case LEDGER_UPGRADE_MAX_TX_SET_SIZE:
        return fmt::format(FMT_STRING("maxtxsetsize={:d}"),
                           upgrade.newMaxTxSetSize());
    case LEDGER_UPGRADE_BASE_RESERVE:
        return fmt::format(FMT_STRING("basereserve={:d}"),
                           upgrade.newBaseReserve());
    case LEDGER_UPGRADE_FLAGS:
        return fmt::format(FMT_STRING("flags={:d}"), upgrade.newFlags());
    case LEDGER_UPGRADE_CONFIG:
        return fmt::format(
            FMT_STRING("{}"),
            xdrToCerealString(upgrade.newConfig(), "configupgradesetkey"));
    case LEDGER_UPGRADE_MAX_SOROBAN_TX_SET_SIZE:
        return fmt::format(FMT_STRING("maxsorobantxsetsize={:d}"),
                           upgrade.newMaxSorobanTxSetSize());
    default:
        return "<unsupported>";
    }
}

std::string
Upgrades::toString() const
{
    std::stringstream r;
    bool first = true;

    auto maybePrintUpgradeTime = [&]() {
        if (first)
        {
            r << fmt::format(
                FMT_STRING("upgradetime={}"),
                VirtualClock::systemPointToISOString(mParams.mUpgradeTime));
            first = false;
        }
    };

    auto appendInfo = [&](std::string const& s,
                          std::optional<uint32> const& o) {
        if (o)
        {
            maybePrintUpgradeTime();
            r << fmt::format(FMT_STRING(", {}={:d}"), s, *o);
        }
    };
    appendInfo("protocolversion", mParams.mProtocolVersion);
    appendInfo("basefee", mParams.mBaseFee);
    appendInfo("basereserve", mParams.mBaseReserve);
    appendInfo("maxtxsetsize", mParams.mMaxTxSetSize);
    appendInfo("maxsorobantxsetsize", mParams.mMaxSorobanTxSetSize);
    appendInfo("flags", mParams.mFlags);
    if (mParams.mConfigUpgradeSetKey)
    {
        maybePrintUpgradeTime();
        r << fmt::format(FMT_STRING(", {}"),
                         xdrToCerealString(*mParams.mConfigUpgradeSetKey,
                                           "configupgradesetkey"));
    }
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
        resetParamIfSet(res.mMaxTxSetSize);
        resetParamIfSet(res.mMaxSorobanTxSetSize);
        resetParamIfSet(res.mBaseReserve);
        resetParamIfSet(res.mFlags);
        if (res.mConfigUpgradeSetKey)
        {
            res.mConfigUpgradeSetKey.reset();
            updated = true;
        }

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
            resetParam(res.mMaxTxSetSize, lu.newMaxTxSetSize());
            break;
        case LEDGER_UPGRADE_BASE_RESERVE:
            resetParam(res.mBaseReserve, lu.newBaseReserve());
            break;
        case LEDGER_UPGRADE_FLAGS:
            resetParam(res.mFlags, lu.newFlags());
            break;
        case LEDGER_UPGRADE_CONFIG:
        {
            if (res.mConfigUpgradeSetKey &&
                *res.mConfigUpgradeSetKey == lu.newConfig())
            {
                res.mConfigUpgradeSetKey.reset();
                updated = true;
            }
            break;
        }
        case LEDGER_UPGRADE_MAX_SOROBAN_TX_SET_SIZE:
            resetParam(res.mMaxSorobanTxSetSize, lu.newMaxSorobanTxSetSize());
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
                          LedgerUpgrade& upgrade, Application& app,
                          LedgerSnapshot const& ls)
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
    auto version = ls.getLedgerHeader().current().ledgerVersion;
    switch (upgrade.type())
    {
    case LEDGER_UPGRADE_VERSION:
    {
        uint32 newVersion = upgrade.newLedgerVersion();
        // only allow upgrades to a supported version of the protocol
        res = res && (newVersion <= app.getConfig().LEDGER_PROTOCOL_VERSION);
        // and enforce versions to be strictly monotonic
        res = res && (newVersion > version);
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
    case LEDGER_UPGRADE_FLAGS:
        res = res &&
              protocolVersionStartsFrom(version, ProtocolVersion::V_18) &&
              (upgrade.newFlags() & ~MASK_LEDGER_HEADER_FLAGS) == 0;
        break;
    case LEDGER_UPGRADE_CONFIG:
    {
        if (protocolVersionIsBefore(version, SOROBAN_PROTOCOL_VERSION))
        {
            return UpgradeValidity::INVALID;
        }
        auto cfgUpgrade =
            ConfigUpgradeSetFrame::makeFromKey(ls, upgrade.newConfig());
        if (!cfgUpgrade)
        {
            return UpgradeValidity::INVALID;
        }
        auto configUpgradeValid = cfgUpgrade->isValidForApply();
        if (configUpgradeValid == UpgradeValidity::XDR_INVALID)
        {
            return UpgradeValidity::XDR_INVALID;
        }
        res = res && (configUpgradeValid == UpgradeValidity::VALID);
        break;
    }
    case LEDGER_UPGRADE_MAX_SOROBAN_TX_SET_SIZE:
        if (protocolVersionIsBefore(version, SOROBAN_PROTOCOL_VERSION))
        {
            return UpgradeValidity::INVALID;
        }
        // Any size is valid.
        break;
    default:
        res = false;
    }

    return res ? UpgradeValidity::VALID : UpgradeValidity::INVALID;
}

bool
Upgrades::isValidForNomination(LedgerUpgrade const& upgrade,
                               LedgerSnapshot const& ls) const
{
    if (!timeForUpgrade(ls.getLedgerHeader().current().scpValue.closeTime))
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
        return mParams.mMaxTxSetSize &&
               (upgrade.newMaxTxSetSize() == *mParams.mMaxTxSetSize);
    case LEDGER_UPGRADE_BASE_RESERVE:
        return mParams.mBaseReserve &&
               (upgrade.newBaseReserve() == *mParams.mBaseReserve);
    case LEDGER_UPGRADE_FLAGS:
        return mParams.mFlags && (upgrade.newFlags() == *mParams.mFlags);
    case LEDGER_UPGRADE_CONFIG:
    {
        if (!mParams.mConfigUpgradeSetKey)
        {
            return false;
        }

        auto cfgUpgrade =
            ConfigUpgradeSetFrame::makeFromKey(ls, upgrade.newConfig());
        return cfgUpgrade &&
               cfgUpgrade->isConsistentWith(ConfigUpgradeSetFrame::makeFromKey(
                   ls, *mParams.mConfigUpgradeSetKey));
    }
    case LEDGER_UPGRADE_MAX_SOROBAN_TX_SET_SIZE:
        return mParams.mMaxSorobanTxSetSize &&
               (upgrade.newMaxSorobanTxSetSize() ==
                *mParams.mMaxSorobanTxSetSize);
    default:
        return false;
    }
}

bool
Upgrades::isValid(UpgradeType const& upgrade, LedgerUpgradeType& upgradeType,
                  bool nomination, Application& app) const
{
    LedgerUpgrade lupgrade;
    auto ls = LedgerSnapshot(app);
    bool res =
        isValidForApply(upgrade, lupgrade, app, ls) == UpgradeValidity::VALID;

    if (nomination)
    {
        res = res && isValidForNomination(lupgrade, ls);
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
    db.getRawSession() << "DROP TABLE IF EXISTS upgradehistory";
    db.getRawSession() << "CREATE TABLE upgradehistory ("
                          "ledgerseq    INT NOT NULL CHECK (ledgerseq >= 0), "
                          "upgradeindex INT NOT NULL, "
                          "upgrade      TEXT NOT NULL, "
                          "changes      TEXT NOT NULL, "
                          "PRIMARY KEY (ledgerseq, upgradeindex)"
                          ")";
    db.getRawSession()
        << "CREATE INDEX upgradehistbyseq ON upgradehistory (ledgerseq);";
}

void
Upgrades::dropSupportUpgradeHistory(Database& db)
{
    db.getRawSession() << "DROP TABLE IF EXISTS upgradehistory";
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
                                  LedgerTxnHeader const& header,
                                  LedgerTxnEntry& offerEntry,
                                  LedgerTxnEntry& accountEntry,
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
    removeEntryWithPossibleSponsorship(ltx, header, offerEntry.current(),
                                       accountEntry);

    offerEntry.erase();
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
prepareLiabilities(AbstractLedgerTxn& ltx, LedgerTxnHeader const& header)
{
    CLOG_INFO(Ledger, "Starting prepareLiabilities");

    auto offersByAccount = ltx.loadAllOffers();

    UnorderedSet<AccountID> changedAccounts;
    uint64_t nChangedTrustLines = 0;

    std::map<UpdateOfferResult, uint64_t> nUpdatedOffers;

    // We want to keep track of the original minBalances before they are changed
    // due to sponsorship changes from deleted offers
    auto offerAccountMinBalanceMap =
        getOfferAccountMinBalances(ltx, header, offersByAccount);

    for (auto& accountOffers : offersByAccount)
    {
        // The purpose of std::unique_ptr here is to have a special value
        // (nullptr) to indicate that an integer overflow would have occurred.
        // Overflow is possible here because existing offers were not
        // constrainted to have int64_t liabilities. This must be carefully
        // handled in what follows.
        std::map<Asset, std::unique_ptr<int64_t>> initialBuyingLiabilities;
        std::map<Asset, std::unique_ptr<int64_t>> initialSellingLiabilities;
        for (auto const& offerEntry : accountOffers.second)
        {
            auto const& offer = offerEntry.current().data.offer();
            addLiabilities(initialBuyingLiabilities, offer.sellerID,
                           offer.buying,
                           getOfferBuyingLiabilities(header, offerEntry));
            addLiabilities(initialSellingLiabilities, offer.sellerID,
                           offer.selling,
                           getOfferSellingLiabilities(header, offerEntry));
        }

        auto accountEntry = stellar::loadAccount(ltx, accountOffers.first);
        if (!accountEntry)
        {
            throw std::runtime_error("account does not exist");
        }
        auto const& acc = accountEntry.current().data.account();
        AccountEntry const accountBefore = acc;

        // balanceAboveReserve must exclude native selling liabilities, since
        // these are in the process of being recalculated from scratch.
        int64_t balance = acc.balance;

        auto it = offerAccountMinBalanceMap.find(accountOffers.first);
        if (it == offerAccountMinBalanceMap.end())
        {
            // this shouldn't happen. getOfferAccountMinBalances added all keys
            // from offersByAccount
            throw std::runtime_error("min balance missing from map");
        }

        int64_t minBalance = it->second;
        int64_t balanceAboveReserve = balance - minBalance;

        std::map<Asset, Liabilities> liabilities;
        for (auto& offerEntry : accountOffers.second)
        {
            auto offerID = offerEntry.current().data.offer().offerID;
            auto res = updateOffer(offerEntry, balance, balanceAboveReserve,
                                   liabilities, initialBuyingLiabilities,
                                   initialSellingLiabilities, ltx, header);
            if (res == UpdateOfferResult::AdjustedToZero ||
                res == UpdateOfferResult::Erase)
            {
                eraseOfferWithPossibleSponsorship(
                    ltx, header, offerEntry, accountEntry, changedAccounts);
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
                    stellar::loadTrustLine(ltx, accountOffers.first, asset);
                int64_t deltaSelling =
                    liab.selling - trustEntry.getSellingLiabilities(header);
                int64_t deltaBuying =
                    liab.buying - trustEntry.getBuyingLiabilities(header);
                if (deltaSelling != 0 || deltaBuying != 0)
                {
                    ++nChangedTrustLines;
                }

                // The deltas could be negative when liabilities were
                // introduced in ledgerVersion 10. This was fixed and
                // ledgerVersion 11 and starting from it deltas should be
                // positive.
                if (protocolVersionStartsFrom(header.current().ledgerVersion,
                                              ProtocolVersion::V_11) &&
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

        if (!(acc == accountBefore))
        {
            changedAccounts.emplace(acc.accountID);
        }
    }

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

static bool
needUpgradeToVersion(ProtocolVersion targetVersion, uint32_t prevVersion,
                     uint32_t newVersion)
{
    return protocolVersionIsBefore(prevVersion, targetVersion) &&
           protocolVersionStartsFrom(newVersion, targetVersion);
}

void
Upgrades::applyVersionUpgrade(Application& app, AbstractLedgerTxn& ltx,
                              uint32_t newVersion)
{
    auto header = ltx.loadHeader();
    uint32_t prevVersion = header.current().ledgerVersion;

    header.current().ledgerVersion = newVersion;
    if (needUpgradeToVersion(ProtocolVersion::V_10, prevVersion, newVersion))
    {
        prepareLiabilities(ltx, header);
    }
    if (protocolVersionEquals(header.current().ledgerVersion,
                              ProtocolVersion::V_16) &&
        protocolVersionEquals(prevVersion, ProtocolVersion::V_15))
    {
        upgradeFromProtocol15To16(ltx);
    }
    if (needUpgradeToVersion(SOROBAN_PROTOCOL_VERSION, prevVersion, newVersion))
    {
        SorobanNetworkConfig::createLedgerEntriesForV20(ltx, app);
#ifdef BUILD_TESTS
        // Update the costs in case if we're in loadgen mode, so that the costs
        // reflect the most recent calibration on p20. This would break
        // if we tried to replay the ledger, but we shouldn't be combining load
        // generation with the ledger replay.
        if (app.getConfig().ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING &&
            app.getConfig()
                .UPDATE_SOROBAN_COSTS_DURING_PROTOCOL_UPGRADE_FOR_TESTING)
        {
            SorobanNetworkConfig::updateRecalibratedCostTypesForV20(ltx);
        }
#endif
    }
    if (needUpgradeToVersion(ProtocolVersion::V_21, prevVersion, newVersion))
    {
        SorobanNetworkConfig::createCostTypesForV21(ltx, app);
    }
    if (needUpgradeToVersion(ProtocolVersion::V_22, prevVersion, newVersion))
    {
        SorobanNetworkConfig::createCostTypesForV22(ltx, app);
    }
    if (needUpgradeToVersion(ProtocolVersion::V_23, prevVersion, newVersion))
    {
        SorobanNetworkConfig::createLedgerEntriesForV23(ltx, app);
    }
}

void
Upgrades::applyReserveUpgrade(AbstractLedgerTxn& ltx, uint32_t newReserve)
{
    auto header = ltx.loadHeader();
    bool didReserveIncrease = newReserve > header.current().baseReserve;

    header.current().baseReserve = newReserve;
    if (protocolVersionStartsFrom(header.current().ledgerVersion,
                                  ProtocolVersion::V_10) &&
        didReserveIncrease)
    {
        prepareLiabilities(ltx, header);
    }
}

ConfigUpgradeSetFrameConstPtr
ConfigUpgradeSetFrame::makeFromKey(LedgerSnapshot const& ls,
                                   ConfigUpgradeSetKey const& key)
{
    auto lk = ConfigUpgradeSetFrame::getLedgerKey(key);
    auto ltxe = ls.load(lk);
    if (!ltxe)
    {
        return nullptr;
    }

    auto ttlLtxe = ls.load(getTTLKey(lk));
    releaseAssert(ttlLtxe);
    if (!isLive(ttlLtxe.current(), ls.getLedgerHeader().current().ledgerSeq))
    {
        return nullptr;
    }

    auto const& contractData = ltxe.current().data.contractData();
    if (contractData.val.type() != SCV_BYTES ||
        contractData.durability != TEMPORARY)
    {
        return nullptr;
    }
    auto const& bytes = contractData.val.bytes();

    ConfigUpgradeSet upgradeSet;
    try
    {
        xdr::xdr_from_opaque(bytes, upgradeSet);
    }
    catch (xdr::xdr_runtime_error&)
    {
        return nullptr;
    }

    return std::shared_ptr<ConfigUpgradeSetFrame>(new ConfigUpgradeSetFrame(
        upgradeSet, key, ls.getLedgerHeader().current().ledgerVersion));
}

ConfigUpgradeSetFrame::ConfigUpgradeSetFrame(
    ConfigUpgradeSet const& upgradeSetXDR, ConfigUpgradeSetKey const& key,
    uint32_t ledgerVersion)
    : mConfigUpgradeSet(upgradeSetXDR)
    , mKey(key)
    , mValidXDR(isValidXDR(upgradeSetXDR, key))
    , mLedgerVersion(ledgerVersion)
{
}

bool
ConfigUpgradeSetFrame::isValidXDR(ConfigUpgradeSet const& upgradeSetXDR,
                                  ConfigUpgradeSetKey const& key) const
{
    if (key.contentHash != sha256(xdr::xdr_to_opaque(upgradeSetXDR)))
    {
        CLOG_DEBUG(Herder,
                   "Got bad configUpgradeSet. Does not match hash in key {}",
                   hexAbbrev(key.contentHash));
        return false;
    }

    if (upgradeSetXDR.updatedEntry.empty())
    {
        CLOG_DEBUG(Herder, "Got bad configUpgradeSet {}: no entries updated",
                   hexAbbrev(key.contentHash));
        return false;
    }

    if (!std::is_sorted(
            upgradeSetXDR.updatedEntry.begin(),
            upgradeSetXDR.updatedEntry.end(),
            [](ConfigSettingEntry const& a, ConfigSettingEntry const& b) {
                return a.configSettingID() < b.configSettingID();
            }))
    {
        CLOG_DEBUG(Herder,
                   "Got bad configUpgradeSet {}: the entries are not ordered",
                   hexAbbrev(key.contentHash));
        return false;
    }
    if (std::adjacent_find(
            upgradeSetXDR.updatedEntry.begin(),
            upgradeSetXDR.updatedEntry.end(),
            [](ConfigSettingEntry const& a, ConfigSettingEntry const& b) {
                return a.configSettingID() == b.configSettingID();
            }) != upgradeSetXDR.updatedEntry.end())
    {
        CLOG_DEBUG(Herder, "Got bad configUpgradeSet {}: duplicate entry",
                   hexAbbrev(key.contentHash));
        return false;
    }
    return true;
}

ConfigUpgradeSet const&
ConfigUpgradeSetFrame::toXDR() const
{
    return mConfigUpgradeSet;
}

ConfigUpgradeSetKey const&
ConfigUpgradeSetFrame::getKey() const
{
    return mKey;
}

LedgerKey
ConfigUpgradeSetFrame::getLedgerKey(ConfigUpgradeSetKey const& upgradeKey)
{
    SCVal v;
    v.type(SCV_BYTES);
    v.bytes().insert(v.bytes().begin(), upgradeKey.contentHash.begin(),
                     upgradeKey.contentHash.end());

    LedgerKey lk;
    lk.type(CONTRACT_DATA);
    lk.contractData().contract.type(SC_ADDRESS_TYPE_CONTRACT);
    lk.contractData().contract.contractId() = upgradeKey.contractID;
    lk.contractData().key = v;
    lk.contractData().durability = TEMPORARY;
    return lk;
}

bool
ConfigUpgradeSetFrame::upgradeNeeded(LedgerSnapshot const& ls) const
{
    if (protocolVersionIsBefore(ls.getLedgerHeader().current().ledgerVersion,
                                SOROBAN_PROTOCOL_VERSION))
    {
        return false;
    }
    for (auto const& updatedEntry : mConfigUpgradeSet.updatedEntry)
    {
        LedgerKey key(LedgerEntryType::CONFIG_SETTING);
        key.configSetting().configSettingID = updatedEntry.configSettingID();
        bool isSame =
            ls.load(key).current().data.configSetting() == updatedEntry;
        if (!isSame)
        {
            return true;
        }
    }
    return false;
}

void
ConfigUpgradeSetFrame::applyTo(AbstractLedgerTxn& ltx) const
{
    for (auto const& updatedEntry : mConfigUpgradeSet.updatedEntry)
    {
        LedgerKey key(LedgerEntryType::CONFIG_SETTING);
        auto const id = updatedEntry.configSettingID();
        key.configSetting().configSettingID = id;
        ltx.load(key).current().data.configSetting() = updatedEntry;
    }
}

bool
ConfigUpgradeSetFrame::isConsistentWith(
    ConfigUpgradeSetFrameConstPtr const& scheduledUpgrade) const
{
    if (scheduledUpgrade == nullptr)
    {
        // We don't have any config upgrades scheduled.
        return false;
    }
    return mKey == scheduledUpgrade->getKey();
}

Upgrades::UpgradeValidity
ConfigUpgradeSetFrame::isValidForApply() const
{
    if (!mValidXDR)
    {
        return Upgrades::UpgradeValidity::XDR_INVALID;
    }
    for (auto const& cfg : mConfigUpgradeSet.updatedEntry)
    {
        if (!SorobanNetworkConfig::isValidConfigSettingEntry(cfg,
                                                             mLedgerVersion) ||
            SorobanNetworkConfig::isNonUpgradeableConfigSettingEntry(cfg))
        {
            return Upgrades::UpgradeValidity::INVALID;
        }
    }
    return Upgrades::UpgradeValidity::VALID;
}

std::string
ConfigUpgradeSetFrame::encodeAsString() const
{
    return decoder::encode_b64(xdr::xdr_to_opaque(mConfigUpgradeSet));
}

std::string
ConfigUpgradeSetFrame::toJson() const
{
    std::ostringstream out;
    cereal::JSONOutputArchive ar(out);
    cereal::save(ar, mConfigUpgradeSet);
    return out.str();
}
}
