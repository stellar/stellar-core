// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/Upgrades.h"
#include "main/Config.h"
#include "util/Timer.h"
#include <xdrpp/marshal.h>

namespace stellar
{

Upgrades::Upgrades(Config const& cfg) : mCfg{cfg}
{
}

std::vector<LedgerUpgrade>
Upgrades::upgradesFor(const LedgerHeader& header) const
{
    auto result = std::vector<LedgerUpgrade>{};
    if (!timeForUpgrade(header.scpValue.closeTime))
    {
        return result;
    }

    if (header.ledgerVersion != mCfg.LEDGER_PROTOCOL_VERSION)
    {
        result.emplace_back(LEDGER_UPGRADE_VERSION);
        result.back().newLedgerVersion() = mCfg.LEDGER_PROTOCOL_VERSION;
    }
    if (header.baseFee != mCfg.DESIRED_BASE_FEE)
    {
        result.emplace_back(LEDGER_UPGRADE_BASE_FEE);
        result.back().newBaseFee() = mCfg.DESIRED_BASE_FEE;
    }
    if (header.maxTxSetSize != mCfg.DESIRED_MAX_TX_PER_LEDGER)
    {
        result.emplace_back(LEDGER_UPGRADE_MAX_TX_SET_SIZE);
        result.back().newMaxTxSetSize() = mCfg.DESIRED_MAX_TX_PER_LEDGER;
    }

    return result;
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
        res = (newVersion == mCfg.LEDGER_PROTOCOL_VERSION);
    }
    break;
    case LEDGER_UPGRADE_BASE_FEE:
    {
        uint32 newFee = lupgrade.newBaseFee();
        res = (newFee == mCfg.DESIRED_BASE_FEE);
    }
    break;
    case LEDGER_UPGRADE_MAX_TX_SET_SIZE:
    {
        uint32 newMax = lupgrade.newMaxTxSetSize();
        res = (newMax == mCfg.DESIRED_MAX_TX_PER_LEDGER);
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
    return mCfg.PREFERRED_UPGRADE_DATETIME <= VirtualClock::from_time_t(time);
}
}
