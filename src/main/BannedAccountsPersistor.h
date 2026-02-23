// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "crypto/KeyUtils.h"
#include "overlay/StellarXDR.h"
#include "util/UnorderedSet.h"
#include <soci.h>
#include <string>
#include <vector>

namespace stellar
{
class Application;

// Manages a persistent set of banned (filtered) accounts. All write operations
// must be called from the main thread.
class BannedAccountsPersistor
{
  public:
    explicit BannedAccountsPersistor(Application& app);

    // Drop and recreate the bannedaccounts table (for new-db initialization).
    static void maybeDropAndCreateNew(soci::session& sess);

    // Add accounts to the ban list (additive, does not remove existing bans).
    void addBannedAccounts(std::vector<std::string> const& addresses);

    // Remove specific accounts from the ban list.
    void removeBannedAccounts(std::vector<std::string> const& addresses);

    // Clear all banned accounts.
    void clearBannedAccounts();

    // Get the set of banned accounts.
    std::set<AccountID> getBannedAccounts();

    // Get the banned accounts as a sorted list of StrKey strings.
    std::vector<std::string> getBannedAccountStrKeys();

  private:
    Application& mApp;
};
}
