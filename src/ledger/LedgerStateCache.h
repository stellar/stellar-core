#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxn.h"
#include "xdr/Stellar-ledger-entries.h"
#include <optional>
#include <shared_mutex>
#include <unordered_set>

namespace stellar
{
// TODO Add ifdef
struct LedgerEntryHash
{
    size_t operator()(LedgerEntry const& entry) const;
};

struct LedgerEntryEqual
{
    bool
    operator()(LedgerEntry const& a, LedgerEntry const& b) const
    {
        return LedgerEntryKey(a) == LedgerEntryKey(b);
    }
};

class LedgerManagerImpl;
class PopulateLedgerCacheWork;

class LedgerStateCache
{
    enum class Mode
    {
        ALL_ENTRIES,
        SOROBAN_ONLY
    };

  private:
    Mode mMode;
    std::unordered_set<LedgerEntry, LedgerEntryHash, LedgerEntryEqual> mState;
    mutable std::shared_mutex mMutex;

    // Add entries to the cache
    // Acquires a unique lock on the cache. Should be called
    // once per ledger, when transfering entries to the bucket list.
    // LedgerEntry in initEntries and liveEntries are added to the cache,
    // while LedgerEntry in deadEntries are removed from the cache.
    void addEntries(std::vector<LedgerEntry> const& initEntries,
                    std::vector<LedgerEntry> const& liveEntries,
                    std::vector<LedgerKey> const& deadEntries);

    // Add a single entry to the cache
    // Acquires a unique lock on the cache. Called
    // when populating the cache.
    void addEntry(LedgerEntry const& entry);

  public:
    LedgerStateCache(Mode mode) : mMode(mode)
    {
    }

    // Read an entry from the cache
    // Acquires a shared lock on the cache.
    std::optional<LedgerEntry> getEntry(LedgerKey const& key) const;

    size_t size() const;
    Mode getMode() const;
    bool supportedKeyType(LedgerEntryType type) const;

    // TODO Find a way to limit access to just the methods we need
    // PopulateLedgerCacheWork::doWork() / addEntry() and
    // LedgerManagerImpl::transferLedgerEntriesToBucketList() / addEntries() I
    // don't think that is possible as they are private/protected themselves,
    // maybe another layer of indirection?
    friend class PopulateLedgerCacheWork;
    friend class LedgerManagerImpl;
};

}