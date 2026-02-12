// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "database/Database.h"
#include "xdr/Stellar-ledger.h"

namespace stellar
{
class Database;

namespace LedgerHeaderUtils
{

uint32_t getFlags(LedgerHeader const& lh);

// Return base64-encoded header data and optionally the hex-encoded hash of the
// header in the hash out parameter. Throws if the header fails basic sanity
// checks (e.g., fee pool > 0).
std::string encodeHeader(LedgerHeader const& header);

#ifdef BUILD_TESTS
std::string encodeHeader(LedgerHeader const& header, std::string& hash);
void storeInDatabase(Database& db, LedgerHeader const& header,
                     SessionWrapper& sess);
#endif

LedgerHeader decodeFromData(std::string const& data);

std::string getHeaderDataForHash(Database& db, Hash const& hash);

void maybeDropAndCreateNew(Database& db);
}
}
