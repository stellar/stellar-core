// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "database/Database.h"
#include "herder/TxSetFrame.h"
#include "overlay/StellarXDR.h"
#include "transactions/TransactionFrameBase.h"

namespace stellar
{
class Application;
class XDROutputFileStream;

void storeTransaction(Database& db, uint32_t ledgerSeq,
                      TransactionFrameBasePtr const& tx,
                      TransactionMeta const& tm,
                      TransactionResultSet const& resultSet);

void storeTxSet(Database& db, uint32_t ledgerSeq, TxSetFrame const& txSet);

void storeTransactionFee(Database& db, uint32_t ledgerSeq,
                         TransactionFrameBasePtr const& tx,
                         LedgerEntryChanges const& changes, uint32_t txIndex);

TransactionResultSet getTransactionHistoryResults(Database& db,
                                                  uint32 ledgerSeq);

std::vector<LedgerEntryChanges> getTransactionFeeMeta(Database& db,
                                                      uint32 ledgerSeq);

size_t copyTransactionsToStream(Application& app, soci::session& sess,
                                uint32_t ledgerSeq, uint32_t ledgerCount,
                                XDROutputFileStream& txOut,
                                XDROutputFileStream& txResultOut);

void createTxSetHistoryTable(Database& db);

void dropTransactionHistory(Database& db);

void deleteOldTransactionHistoryEntries(Database& db, uint32_t ledgerSeq,
                                        uint32_t count);

void deleteNewerTransactionHistoryEntries(Database& db, uint32_t ledgerSeq);
}
