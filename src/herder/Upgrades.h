#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "xdr/Stellar-ledger.h"

#include <stdint.h>
#include <vector>

namespace stellar
{
class Config;
struct LedgerHeader;
struct LedgerUpgrade;

class Upgrades
{
  public:
    explicit Upgrades(Config const& cfg);

    // create upgrades for given ledger
    std::vector<LedgerUpgrade> upgradesFor(LedgerHeader const& header) const;

    // apply upgrade to ledger header
    static void applyTo(LedgerUpgrade const& upgrade, LedgerHeader& header);

    // convert upgrade value to string
    static std::string toString(LedgerUpgrade const& upgrade);

    // returns true if upgrade is a valid upgrade step
    // in which case it also sets upgradeType
    bool isValid(uint64_t closeTime, UpgradeType const& upgrade,
                 LedgerUpgradeType& upgradeType) const;

  private:
    Config const& mCfg;

    bool timeForUpgrade(uint64_t time) const;
};
}
