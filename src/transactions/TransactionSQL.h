// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "database/Database.h"
#include "overlay/StellarXDR.h"
#include "transactions/TransactionFrameBase.h"

namespace stellar
{
class XDROutputFileStream;

void storeTransaction(Database& db, uint32_t ledgerSeq,
                      TransactionFrameBasePtr const& tx, TransactionMeta& tm,
                      TransactionResultSet const& resultSet);

void storeTransactionFee(Database& db, uint32_t ledgerSeq,
                         TransactionFrameBasePtr const& tx,
                         LedgerEntryChanges const& changes, uint32_t txIndex);

TransactionResultSet getTransactionHistoryResults(Database& db,
                                                  uint32 ledgerSeq);

std::vector<LedgerEntryChanges> getTransactionFeeMeta(Database& db,
                                                      uint32 ledgerSeq);

size_t copyTransactionsToStream(Hash const& networkID, Database& db,
                                soci::session& sess, uint32_t ledgerSeq,
                                uint32_t ledgerCount,
                                XDROutputFileStream& txOut,
                                XDROutputFileStream& txResultOut);

void dropTransactionHistory(Database& db);

void deleteOldTransactionHistoryEntries(Database& db, uint32_t ledgerSeq,
                                        uint32_t count);
}
