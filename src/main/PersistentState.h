// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "database/Database.h"
#include "main/Application.h"
#include "xdr/Stellar-internal.h"
#include <string>
#include <unordered_set>

namespace stellar
{

// PersistentState class ensures all state critical to node health, such as
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
        kRebuildLedger,
        kLastEntryMain,

        // SCP-related entries
        kMiscDatabaseSchema,
        kLedgerUpgrades,
        kLastSCPDataXDR,
        kTxSet,
        kLastEntry,
    };

    static void maybeDropAndCreateNew(Database& db);
    static void createMisc(Database& db);

    std::string getState(Entry stateName, SessionWrapper& session);
    void setMainState(Entry stateName, std::string const& value,
                      SessionWrapper& session);
    void setMiscState(Entry stateName, std::string const& value);

    // Special methods for SCP state (multiple slots)
    std::unordered_map<uint32_t, std::string> getSCPStateAllSlots();
    std::unordered_map<Hash, std::string> getTxSetsForAllSlots();
    std::unordered_set<Hash> getTxSetHashesForAllSlots();

    void
    setSCPStateV1ForSlot(uint64 slot, std::string const& value,
                         std::unordered_map<Hash, std::string> const& txSets);

    bool shouldRebuildForOfferTable();
    void clearRebuildForOfferTable();
    void setRebuildForOfferTable();

    bool hasTxSet(Hash const& txSetHash);
    void deleteTxSets(std::unordered_set<Hash> hashesToDelete);
    static std::string getStoreStateName(Entry n, uint32 subscript = 0);

  private:
    static std::string kSQLCreateStatement;
    static std::string kSQLCreateSCPStatement;
    static std::string mainMapping[kLastEntryMain];
    static std::string miscMapping[kLastEntry];
    static std::string kLCLTableName;
    static std::string kSlotTableName;

    Application& mApp;

    static std::string getStoreStateNameForTxSet(Hash const& txSetHash);

    void setSCPStateForSlot(uint64 slot, std::string const& value);
    void updateDb(std::string const& entry, std::string const& value,
                  SessionWrapper& session, std::string const& tableName);

    std::string getFromDb(std::string const& entry, SessionWrapper& session,
                          std::string const& tableName);
    static std::string getTableForEntry(PersistentState::Entry entry);
};
}
