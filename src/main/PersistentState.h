#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/TxSetFrame.h"
#include "main/Application.h"
#include "xdr/Stellar-internal.h"
#include <string>

namespace stellar
{

class PersistentState
{
  public:
    PersistentState(Application& app);

    enum Entry
    {
        // Main database entries
        kLastClosedLedger,
        kHistoryArchiveState,
        kDatabaseSchema,
        kNetworkPassphrase,
        kRebuildLedger,
        kDBBackend,
        kLastEntryMain, // Marker for the end of main database entries

        // Misc database entries
        kLastSCPData = kLastEntryMain + 1,
        kLedgerUpgrades,
        kLastSCPDataXDR,
        kTxSet,
        kMiscDatabaseSchema,
        kLastEntry // Marker for the end of misc database entries
    };

    static void dropAll(Database& db);
    static void dropMisc(Database& db);

    std::string getState(Entry stateName, SessionWrapper& session);
    void setState(Entry stateName, std::string const& value,
                  SessionWrapper& session);

    // Special methods for SCP state (multiple slots)
    std::vector<std::string> getSCPStateAllSlots();
    std::vector<std::string> getTxSetsForAllSlots();
    std::unordered_set<Hash> getTxSetHashesForAllSlots();

    void
    setSCPStateV1ForSlot(uint64 slot, std::string const& value,
                         std::unordered_map<Hash, std::string> const& txSets);

    bool shouldRebuildForType(LedgerEntryType let);
    void clearRebuildForType(LedgerEntryType let);
    void setRebuildForType(LedgerEntryType let);

    bool hasTxSet(Hash const& txSetHash);
    void deleteTxSets(std::unordered_set<Hash> hashesToDelete);
    std::string getStoreStateName(Entry n, uint32 subscript = 0) const;

  private:
    static std::string kSQLCreateStatement;
    static std::string mainMapping[kLastEntryMain];
    static std::string miscMapping[kLastEntry];

    Application& mApp;

    std::string getStoreStateNameForTxSet(Hash const& txSetHash);

    void setSCPStateForSlot(uint64 slot, std::string const& value);
    void updateDb(std::string const& entry, std::string const& value,
                  SessionWrapper& session);

    std::string getFromDb(std::string const& entry, SessionWrapper& session);

    bool entryExists(std::string const& entry, SessionWrapper& session);
};
}
