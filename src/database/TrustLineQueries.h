#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/optional.h"
#include "xdr/Stellar-ledger-entries.h"

#include <unordered_map>
#include <vector>

namespace stellar
{

class Database;
struct LedgerKey;

void createTrustLinesTable(Database& db);
optional<LedgerEntry const> selectTrustLine(AccountID const& accountID,
                                            Asset const& asset, Database& db);
void insertTrustLine(LedgerEntry const& entry, Database& db);
void updateTrustLine(LedgerEntry const& entry, Database& db);
bool trustLineExists(LedgerKey const& key, Database& db);
void deleteTrustLine(LedgerKey const& key, Database& db);

std::unordered_map<AccountID, int> selectTrustLineCountPerAccount(Database& db);
uint64_t countTrustLines(Database& db);
uint64_t countTrustLines(AccountID const& accountID, Database& db);
}
