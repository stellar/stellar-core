#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "xdr/Stellar-ledger.h"

#include "main/Application.h"
#include "main/Config.h"
#include "util/Timer.h"
#include <optional>
#include <stdint.h>
#include <vector>

namespace stellar
{
class AbstractLedgerTxn;
class Config;
class Database;
struct LedgerHeader;
struct LedgerUpgrade;
class LedgerSnapshot;

class ConfigUpgradeSetFrame;
using ConfigUpgradeSetFrameConstPtr =
    std::shared_ptr<ConfigUpgradeSetFrame const>;

class Upgrades
{
  public:
    // # of hours after the scheduled upgrade time before we remove pending
    // upgrades
    static std::chrono::hours const UPDGRADE_EXPIRATION_HOURS;

    struct UpgradeParameters
    {
        UpgradeParameters()
        {
        }
        UpgradeParameters(Config const& cfg)
        {
            mUpgradeTime = cfg.TESTING_UPGRADE_DATETIME;
            mProtocolVersion = std::make_optional<uint32>(
                cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION);
            mBaseFee =
                std::make_optional<uint32>(cfg.TESTING_UPGRADE_DESIRED_FEE);
            mMaxTxSetSize =
                std::make_optional<uint32>(cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE);
            mBaseReserve =
                std::make_optional<uint32>(cfg.TESTING_UPGRADE_RESERVE);
            mFlags = std::make_optional<uint32>(cfg.TESTING_UPGRADE_FLAGS);
        }
        VirtualClock::system_time_point mUpgradeTime;
        std::optional<uint32> mProtocolVersion;
        std::optional<uint32> mBaseFee;
        std::optional<uint32> mMaxTxSetSize;
        std::optional<uint32> mBaseReserve;
        std::optional<uint32> mFlags;
        std::optional<uint32> mMaxSorobanTxSetSize;
        std::optional<ConfigUpgradeSetKey> mConfigUpgradeSetKey;

        std::string toJson() const;
        void fromJson(std::string const& s);
        std::string toDebugJson(LedgerSnapshot const& ls) const;
    };

    Upgrades()
    {
    }
    explicit Upgrades(UpgradeParameters const& params);

    void setParameters(UpgradeParameters const& params, Config const& cfg);

    UpgradeParameters const& getParameters() const;

    // create upgrades for given ledger
    std::vector<LedgerUpgrade>
    createUpgradesFor(LedgerHeader const& lclHeader,
                      LedgerSnapshot const& ls) const;

    // apply upgrade to ledger header
    static void applyTo(LedgerUpgrade const& upgrade, Application& app,
                        AbstractLedgerTxn& ltx);

    // convert upgrade value to string
    static std::string toString(LedgerUpgrade const& upgrade);

    enum class UpgradeValidity
    {
        VALID,
        XDR_INVALID,
        INVALID
    };

    // VALID if it is safe to apply upgrade
    // XDR_INVALID if the upgrade cannot be deserialized
    // INVALID if it is unsafe to apply the upgrade for any other reason
    //
    // If the upgrade could be deserialized then lupgrade is set
    static UpgradeValidity isValidForApply(UpgradeType const& upgrade,
                                           LedgerUpgrade& lupgrade,
                                           Application& app,
                                           LedgerSnapshot const& ls);

    // returns true if upgrade is a valid upgrade step
    // in which case it also sets upgradeType
    bool isValid(UpgradeType const& upgrade, LedgerUpgradeType& upgradeType,
                 bool nomination, Application& app) const;

    // constructs a human readable string that represents
    // the pending upgrades
    std::string toString() const;

    // sets updated to true if some upgrades were removed
    UpgradeParameters
    removeUpgrades(std::vector<UpgradeType>::const_iterator beginUpdates,
                   std::vector<UpgradeType>::const_iterator endUpdates,
                   uint64_t time, bool& updated);

    static void dropAll(Database& db);
    static void dropSupportUpgradeHistory(Database& db);

  private:
    UpgradeParameters mParams;

    bool timeForUpgrade(uint64_t time) const;

    // returns true if upgrade is a valid upgrade step
    // in which case it also sets lupgrade
    bool isValidForNomination(LedgerUpgrade const& upgrade,
                              LedgerSnapshot const& ls) const;

    static void applyVersionUpgrade(Application& app, AbstractLedgerTxn& ltx,
                                    uint32_t newVersion);

    static void applyReserveUpgrade(AbstractLedgerTxn& ltx,
                                    uint32_t newReserve);
};

// ConfigUpgradeSetFrame contains a ConfigUpgradeSet that
// was retrieved from the ledger given a ConfigUpgradeSetKey. The
// ConfigUpgradeSetKey will be converted to a ContractData LedgerKey, and the
// ContractData LedgerEntry retreived with that will have a val of SCV_BYTES
// containing a serialized ConfigUpgradeSet. The ConfigUpgradeSetKey is what
// validators vote on during upgrades. The hash in the key must match the sha256
// hash of the ConfigUpgradeSet.
class ConfigUpgradeSetFrame
{
  public:
    static ConfigUpgradeSetFrameConstPtr
    makeFromKey(LedgerSnapshot const& ls, ConfigUpgradeSetKey const& key);

    static LedgerKey getLedgerKey(ConfigUpgradeSetKey const& upgradeKey);

    ConfigUpgradeSet const& toXDR() const;

    ConfigUpgradeSetKey const& getKey() const;

    bool upgradeNeeded(LedgerSnapshot const& ls) const;

    void applyTo(AbstractLedgerTxn& ltx) const;

    bool isConsistentWith(
        ConfigUpgradeSetFrameConstPtr const& scheduledUpgrade) const;

    Upgrades::UpgradeValidity isValidForApply() const;

    std::string encodeAsString() const;

    std::string toJson() const;

  private:
    ConfigUpgradeSetFrame(ConfigUpgradeSet const& upgradeSetXDR,
                          ConfigUpgradeSetKey const& key,
                          uint32_t ledgerVersion);

    bool isValidXDR(ConfigUpgradeSet const& upgradeSetXDR,
                    ConfigUpgradeSetKey const& key) const;

    ConfigUpgradeSet mConfigUpgradeSet;
    ConfigUpgradeSetKey mKey;
    bool mValidXDR;
    uint32_t const mLedgerVersion;
};
}
