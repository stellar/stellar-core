// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "xdr/Stellar-ledger.h"

namespace stellar
{
class Database;
class SessionWrapper;

namespace LedgerHeaderUtils
{

uint32_t getFlags(LedgerHeader const& lh);

// Return base64-encoded header data. Throws if the header fails basic sanity
// checks (e.g., fee pool >= 0).
std::string encodeHeader(LedgerHeader const& header);

#ifdef BUILD_TESTS
// Like the non-test encodeHeader, except also include the hex-encoded hash of
// the header in the `hash` out parameter
std::string encodeHeader(LedgerHeader const& header, std::string& hash);
void storeInDatabase(Database& db, LedgerHeader const& header,
                     SessionWrapper& sess);
#endif

LedgerHeader decodeFromData(std::string const& data);

// Returns the base64-encoded header data for the given hash. Returns an empty
// string if no header is found for the hash.
std::string getHeaderDataForHash(Database& db, Hash const& hash);

void maybeDropAndCreateNew(Database& db);
}
}
