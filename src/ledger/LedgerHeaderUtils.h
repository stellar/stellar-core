#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "database/Database.h"
#include "xdr/Stellar-ledger.h"

namespace stellar
{
class XDROutputFileStream;

bool isLedgerHeaderValid(LedgerHeader const& lh);

void storeHeaderInDatabase(Database& db, LedgerHeader const& header);

LedgerHeader decodeLedgerHeaderFromData(std::string const& data);

std::shared_ptr<LedgerHeader>
loadLedgerHeaderByHash(Database& db, Hash const& hash);

std::shared_ptr<LedgerHeader>
loadLedgerHeaderBySequence(Database& db, soci::session& sess, uint32_t seq);

void deleteOldLedgerHeaders(Database& db, uint32_t ledgerSeq, uint32_t count);

size_t copyLedgerHeadersToStream(
        Database& db, soci::session& sess, uint32_t ledgerSeq,
        uint32_t ledgerCount, XDROutputFileStream& headersOut);
}
