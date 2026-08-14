// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "ledger/LedgerEntryScope.h"
#include "ledger/LedgerHashUtils.h"
#include "ledger/LedgerTxn.h"
#include "util/UnorderedMap.h"
#include "util/UnorderedSet.h"
#include "xdr/Stellar-ledger-entries.h"

namespace stellar
{

// A LedgerKey that caches its own hash.
//
// The parallel apply maps are keyed by LedgerKey and the same key is looked
// up many times as it flows tx map -> thread map -> global map, so caching
// the hash avoids repeated re-hashing of the same key.
class ParallelApplyLedgerKey
{
  public:
    explicit ParallelApplyLedgerKey(LedgerKey const& ledgerKey)
        : mLedgerKey(ledgerKey), mHash(std::hash<LedgerKey>{}(mLedgerKey))
    {
    }
    explicit ParallelApplyLedgerKey(LedgerKey&& ledgerKey)
        : mLedgerKey(std::move(ledgerKey))
        , mHash(std::hash<LedgerKey>{}(mLedgerKey))
    {
    }

    LedgerKey const&
    ledgerKey() const
    {
        return mLedgerKey;
    }

    operator LedgerKey const&() const
    {
        return mLedgerKey;
    }

    size_t
    hash() const
    {
        return mHash;
    }

  private:
    LedgerKey mLedgerKey;
    size_t mHash;
};

inline bool
operator==(ParallelApplyLedgerKey const& lhs, ParallelApplyLedgerKey const& rhs)
{
    return lhs.ledgerKey() == rhs.ledgerKey();
}

using ParallelApplyLedgerKeySet = UnorderedSet<ParallelApplyLedgerKey>;
template <typename T>
using ParallelApplyLedgerKeyMap = UnorderedMap<ParallelApplyLedgerKey, T>;

// Tracks entry updates within a transaction during parallel apply phases. If
// the transaction succeeds, the thread's ParallelApplyEntryMap should be
// updated with the entries from the TxModifiedEntryMap.
using TxParApplyLedgerEntry =
    ScopedLedgerEntry<StaticLedgerEntryScope::TxParApply>;
using TxModifiedEntryMap = ParallelApplyLedgerKeyMap<TxParApplyLedgerEntryOpt>;

// Used to track the current state of an entry during parallel apply phases. Can
// be updated by successful transactions.
template <StaticLedgerEntryScope S> struct ParallelApplyEntry
{
    // Will not be set if the entry doesn't exist, or if no tx was able to load
    // it due to hitting read limits.
    ScopedLedgerEntryOpt<S> mLedgerEntry;
    bool mIsDirty;
    // Whether the entry does not exist in the ledger state that the parallel
    // apply phase started from, i.e. it is being created during the phase.
    bool mIsNew;

    ParallelApplyEntry(ScopedLedgerEntryOpt<S> ledgerEntry, bool isDirty,
                       bool isNew)
        : mLedgerEntry(std::move(ledgerEntry)), mIsDirty(isDirty), mIsNew(isNew)
    {
    }

    static ParallelApplyEntry
    clean(ScopedLedgerEntryOpt<S> e, bool isNew)
    {
        return ParallelApplyEntry(std::move(e), false, isNew);
    }
    static ParallelApplyEntry
    dirty(ScopedLedgerEntryOpt<S> e, bool isNew)
    {
        return ParallelApplyEntry(std::move(e), true, isNew);
    }
    template <StaticLedgerEntryScope S2>
    ParallelApplyEntry<S2>
    rescope(LedgerEntryScope<S> const& s1,
            LedgerEntryScope<S2> const& s2) const&
    {
        return ParallelApplyEntry<S2>(
            s2.scopeAdoptEntryOptFrom(mLedgerEntry, s1), mIsDirty, mIsNew);
    }
    // Moves the entry payload into the new scope and thus makes the current
    // entry invalid.
    template <StaticLedgerEntryScope S2>
    ParallelApplyEntry<S2>
    rescope(LedgerEntryScope<S> const& s1, LedgerEntryScope<S2> const& s2) &&
    {
        return ParallelApplyEntry<S2>(
            s2.scopeAdoptEntryOptFrom(std::move(mLedgerEntry), s1), mIsDirty,
            mIsNew);
    }
};
using GlobalParallelApplyEntry =
    ParallelApplyEntry<StaticLedgerEntryScope::GlobalParApply>;
using ThreadParallelApplyEntry =
    ParallelApplyEntry<StaticLedgerEntryScope::ThreadParApply>;
using TxParallelApplyEntry =
    ParallelApplyEntry<StaticLedgerEntryScope::TxParApply>;

// This is a map of all entries that will be read and/or written during parallel
// apply phases: there is one such "global" map which disjoint per-thread maps
// get split off of, modified during applyThread, and merged back into. Once all
// threads return, the updates from each threads entry map should be committed
// to LedgerTxn.
template <StaticLedgerEntryScope S>
using ParallelApplyEntryMap = ParallelApplyLedgerKeyMap<ParallelApplyEntry<S>>;
using GlobalParallelApplyEntryMap =
    ParallelApplyEntryMap<StaticLedgerEntryScope::GlobalParApply>;
using ThreadParallelApplyEntryMap =
    ParallelApplyEntryMap<StaticLedgerEntryScope::ThreadParApply>;
using TxParallelApplyEntryMap =
    ParallelApplyEntryMap<StaticLedgerEntryScope::TxParApply>;

// Returned by each parallel transaction on success. It will contain the entries
// modified by the transaction and the keys restored.
class ParallelTxSuccessVal
    : public LedgerEntryScope<StaticLedgerEntryScope::TxParApply>
{
  public:
    ParallelTxSuccessVal(TxModifiedEntryMap&& modifiedEntryMap,
                         ScopeIdT txScopeID)
        : LedgerEntryScope(txScopeID)
        , mModifiedEntryMap(std::move(modifiedEntryMap))
    {
        // The ModifiedEntryMap should not be used for reading entries, only
        // to serve as a source for thread state to scopeAdoptEntryFrom. So
        // we deactivate ourselves as a LedgerEntryScope on construction, to
        // prevent accidental reads.
        scopeDeactivate();
    }
    ParallelTxSuccessVal(TxModifiedEntryMap&& modifiedEntryMap,
                         RestoredEntries&& restoredEntries, ScopeIdT txScopeID)
        : LedgerEntryScope(txScopeID)
        , mModifiedEntryMap(std::move(modifiedEntryMap))
        , mRestoredEntries(std::move(restoredEntries))
    {
        scopeDeactivate();
    }

    TxModifiedEntryMap const&
    getModifiedEntryMap() const
    {
        return mModifiedEntryMap;
    }
    // Mutable access to the modified entry map. This should only be used to
    // consume the map when committing the result to the thread state.
    TxModifiedEntryMap&
    getModifiedEntryMapMut()
    {
        return mModifiedEntryMap;
    }
    RestoredEntries const&
    getRestoredEntries() const
    {
        return mRestoredEntries;
    }

    friend class TxParallelApplyLedgerState;

  private:
    // This will contain a key for every entry modified by a transaction
    TxModifiedEntryMap mModifiedEntryMap;
    RestoredEntries mRestoredEntries;
};
} // namespace stellar

namespace std
{
template <> class hash<stellar::ParallelApplyLedgerKey>
{
  public:
    size_t
    operator()(stellar::ParallelApplyLedgerKey const& key) const
    {
        return key.hash();
    }
};
} // namespace std
