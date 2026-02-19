// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "database/Database.h"
#include "xdr/Stellar-ledger.h"

namespace stellar
{

namespace LedgerHeaderUtils
{

uint32_t getFlags(LedgerHeader const& lh);

void storeInDatabase(Database& db, LedgerHeader const& header,
                     SessionWrapper& sess);

std::shared_ptr<LedgerHeader> loadByHash(Database& db, Hash const& hash);

void deleteOldEntries(soci::session& sess, uint32_t ledgerSeq, uint32_t count);

void maybeDropAndCreateNew(Database& db);
}
}
