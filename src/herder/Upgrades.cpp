// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/Upgrades.h"
#include "main/Config.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include <lib/util/format.h>
#include <xdrpp/marshal.h>

namespace stellar
{

Upgrades::Upgrades(UpgradeParameters const& params) : mParams(params)
{
}

void
Upgrades::setParameters(UpgradeParameters const& params)
{
    mParams = params;
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
Upgrades::applyTo(LedgerUpgrade const& upgrade, LedgerHeader& header)
{
    switch (upgrade.type())
    {
    case LEDGER_UPGRADE_VERSION:
        header.ledgerVersion = upgrade.newLedgerVersion();
        break;
    case LEDGER_UPGRADE_BASE_FEE:
        header.baseFee = upgrade.newBaseFee();
        break;
    case LEDGER_UPGRADE_MAX_TX_SET_SIZE:
        header.maxTxSetSize = upgrade.newMaxTxSetSize();
        break;
    case LEDGER_UPGRADE_BASE_RESERVE:
        header.baseReserve = upgrade.newBaseReserve();
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
        return fmt::format("PROTOCOL_VERSION={0}", upgrade.newLedgerVersion());
    case LEDGER_UPGRADE_BASE_FEE:
        return fmt::format("BASE_FEE={0}", upgrade.newBaseFee());
    case LEDGER_UPGRADE_MAX_TX_SET_SIZE:
        return fmt::format("MAX_TX_SET_SIZE={0}", upgrade.newMaxTxSetSize());
    case LEDGER_UPGRADE_BASE_RESERVE:
        return fmt::format("BASE_RESERVE={0}", upgrade.newBaseReserve());
    default:
        return "<unsupported>";
    }
}

std::string
Upgrades::toString(std::vector<LedgerUpgrade> const& upgrades)
{
    if (upgrades.empty())
    {
        return {};
    }

    auto result = std::string{};
    for (auto const& upgrade : upgrades)
    {
        if (!result.empty())
        {
            result += ", ";
        }
        result += toString(upgrade);
    }

    return fmt::format("[{0}]", result);
}

std::string
Upgrades::toString(LedgerHeader const& header)
{
    return fmt::format("PROTOCOL_VERSION={0}, BASE_FEE={1}, "
                       "MAX_TX_SET_SIZE={2}, BASE_RESERVE={3}",
                       header.ledgerVersion, header.baseFee,
                       header.maxTxSetSize, header.baseReserve);
}

bool
Upgrades::isValid(uint64_t closeTime, UpgradeType const& upgrade,
                  LedgerUpgradeType& upgradeType) const
{
    if (!timeForUpgrade(closeTime))
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

    bool res;
    switch (lupgrade.type())
    {
    case LEDGER_UPGRADE_VERSION:
    {
        uint32 newVersion = lupgrade.newLedgerVersion();
        res = mParams.mProtocolVersion &&
              (newVersion == *mParams.mProtocolVersion);
    }
    break;
    case LEDGER_UPGRADE_BASE_FEE:
    {
        uint32 newFee = lupgrade.newBaseFee();
        res = mParams.mBaseFee && (newFee == *mParams.mBaseFee);
    }
    break;
    case LEDGER_UPGRADE_MAX_TX_SET_SIZE:
    {
        uint32 newMax = lupgrade.newMaxTxSetSize();
        res = mParams.mMaxTxSize && (newMax == *mParams.mMaxTxSize);
    }
    break;
    case LEDGER_UPGRADE_BASE_RESERVE:
    {
        uint32 newReserve = lupgrade.newBaseReserve();
        res = mParams.mBaseReserve && (newReserve == *mParams.mBaseReserve);
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
}
