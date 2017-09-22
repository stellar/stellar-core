#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/optional.h"
#include "xdr/Stellar-ledger-entries.h"

#include <cstdint>

namespace stellar
{

class Database;
struct LedgerKey;

struct NumberOfSubentries
{
    uint64_t inAccountsTable;
    uint64_t calculated;
};

void createAccountsTable(Database& db);
std::vector<LedgerEntry> selectAllAccounts(Database& db);
optional<LedgerEntry const> selectAccount(AccountID const& accountID,
                                          Database& db);
void insertAccount(LedgerEntry const& entry, Database& db);
void updateAccount(LedgerEntry const& entry, Database& db);
bool accountExists(LedgerKey const& key, Database& db);
void deleteAccount(LedgerKey const& key, Database& db);
uint64_t countAccounts(Database& db);

uint64_t sumOfBalances(Database& db);
NumberOfSubentries numberOfSubentries(AccountID const& accountID, Database& db);
}
