#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "database/Database.h"
#include "main/Application.h"
#include "xdr/Stellar-internal.h"
#include <string>
#include <unordered_set>

namespace stellar
{

// PersistentState class ensures all state criticial to node health, such as
// LCL, SCP messages, upgrades etc are persisted to the database. The class
// maintains two separate tables to avoid conflicts during concurrent writes.
class PersistentState
{
  public:
    PersistentState(Application& app);

    enum Entry
    {
        // LCL-related entries
        kLastClosedLedger = 0,
        kHistoryArchiveState,
        kDatabaseSchema,
        kNetworkPassphrase,
        // https://github.com/stellar/stellar-core/issues/4582
        kDBBackend,
        kRebuildLedger,
        // SCP-related entries
        kLedgerUpgrades,
        kLastSCPDataXDR,
        kTxSet,
        kLastEntry,
    };

    static void dropAll(Database& db);

    std::string getState(Entry stateName, SessionWrapper& session);
    void setState(Entry stateName, std::string const& value,
                  SessionWrapper& session);

    // Special methods for SCP state (multiple slots)
    std::unordered_map<uint32_t, std::string>
    getSCPStateAllSlots(std::string table = kSlotTableName);
    std::unordered_map<Hash, std::string>
    getTxSetsForAllSlots(std::string table = kSlotTableName);
    std::unordered_set<Hash> getTxSetHashesForAllSlots();

    void
    setSCPStateV1ForSlot(uint64 slot, std::string const& value,
                         std::unordered_map<Hash, std::string> const& txSets);

    bool shouldRebuildForOfferTable();
    void clearRebuildForOfferTable();
    void setRebuildForOfferTable();

    bool hasTxSet(Hash const& txSetHash);
    void deleteTxSets(std::unordered_set<Hash> hashesToDelete,
                      std::string table = kSlotTableName);
    void migrateToSlotStateTable();

  private:
    static std::string kSQLCreateStatement;
    static std::string kSQLCreateSCPStatement;
    static std::string mapping[kLastEntry];
    static std::string kLCLTableName;
    static std::string kSlotTableName;

    Application& mApp;

    static std::string getStoreStateName(Entry n, uint32 subscript = 0);
    static std::string getStoreStateNameForTxSet(Hash const& txSetHash);

    void setSCPStateForSlot(uint64 slot, std::string const& value);
    void updateDb(std::string const& entry, std::string const& value,
                  SessionWrapper& session, std::string const& tableName);

    std::string getFromDb(std::string const& entry, SessionWrapper& session,
                          std::string const& tableName);
    static std::string getDBForEntry(PersistentState::Entry entry);
};
}
