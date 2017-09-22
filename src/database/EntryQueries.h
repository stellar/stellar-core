#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/optional.h"
#include "xdr/Stellar-SCP.h"
#include "xdr/Stellar-ledger-entries.h"

namespace stellar
{

class Database;
struct LedgerKey;

optional<LedgerEntry const> selectEntry(LedgerKey const& key, Database& db);
void insertEntry(LedgerEntry const& entry, Database& db);
void updateEntry(LedgerEntry const& entry, Database& db);
bool entryExists(LedgerKey const& key, Database& db);
void deleteEntry(LedgerKey const& key, Database& db);
}
