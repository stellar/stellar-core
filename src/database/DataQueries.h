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
class StatementContext;
struct LedgerKey;

void createDataTable(Database& db);
optional<LedgerEntry const> selectData(AccountID const& accountID,
                                       std::string dataName, Database& db);
void insertData(LedgerEntry const& entry, Database& db);
void updateData(LedgerEntry const& entry, Database& db);
bool dataExists(LedgerKey const& key, Database& db);
void deleteData(LedgerKey const& key, Database& db);

std::unordered_map<AccountID, int> selectDataCountPerAccount(Database& db);
uint64_t countData(Database& db);
}
