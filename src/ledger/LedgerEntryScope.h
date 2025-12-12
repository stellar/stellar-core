#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "xdr/Stellar-ledger-entries.h"
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <optional>

#define FOREACH_STATIC_LEDGER_ENTRY_SCOPE(MACRO) \
    MACRO(GLOBAL_PAR_APPLY_STATE) \
    MACRO(THREAD_PAR_APPLY_STATE) \
    MACRO(TX_PAR_APPLY_STATE) \
    MACRO(LIVE_BUCKET_LIST) \
    MACRO(HOT_ARCHIVE_BUCKET_LIST) \
    MACRO(EVICTION_SCAN) \
    MACRO(LIVE_SOROBAN_IN_MEMORY_STATE)

namespace stellar
{

enum class StaticLedgerEntryScope : uint16_t
{
#define STATIC_SCOPE_MACRO(SCOPE_NAME) SCOPE_NAME,
    FOREACH_STATIC_LEDGER_ENTRY_SCOPE(STATIC_SCOPE_MACRO)
#undef STATIC_SCOPE_MACRO
};

template <StaticLedgerEntryScope> class LedgerEntryScopeID;

}

template <stellar::StaticLedgerEntryScope T>
std::ostream& operator<<(std::ostream& os,
                         stellar::LedgerEntryScopeID<T> const& obj);

namespace stellar
{

// LedgerEntryScopeID uniquely identifies a scope for ledger entries. It is
// space-optimized to a single 64 bit word since these are stuck on all
// ScopedLedgerEntry instances and there might be a lot of those.
template <StaticLedgerEntryScope S> class LedgerEntryScopeID
{
  public:
    static constexpr StaticLedgerEntryScope staticScope = S;
    uint16_t const mIndex;
    uint32_t const mLedger;

    // NB: index is limited to 16 bits to keep size down;
    // the constructor will throw if you exceed that, but
    // in practice scopes should always be tiny numbers like
    // the thread cluster index or the bucketlist level.
    LedgerEntryScopeID(size_t index, uint32_t ledger);

    bool operator==(LedgerEntryScopeID const& other) const;
    bool operator!=(LedgerEntryScopeID const& other) const;

    friend std::ostream& ::operator<<(std::ostream& os,
                                      LedgerEntryScopeID const& obj);
};

static_assert(
    sizeof(
        LedgerEntryScopeID<StaticLedgerEntryScope::GLOBAL_PAR_APPLY_STATE>) ==
        8,
    "Unexpected size for LedgerEntryScopeID");

template <StaticLedgerEntryScope T> class LedgerEntryScope;
template <StaticLedgerEntryScope S> class ScopedOptionalLedgerEntry;

template <StaticLedgerEntryScope S> class ScopedLedgerEntry
{
    static constexpr StaticLedgerEntryScope staticScope = S;
    using ScopeIdT = LedgerEntryScopeID<S>;
    using ScopeT = LedgerEntryScope<S>;
    LedgerEntry mEntry;

    friend class LedgerEntryScope<S>;
    friend class ScopedOptionalLedgerEntry<S>;
#define FRIEND_MACRO(SCOPE) \
    friend class LedgerEntryScope<StaticLedgerEntryScope::SCOPE>;
    FOREACH_STATIC_LEDGER_ENTRY_SCOPE(FRIEND_MACRO)
#undef FRIEND_MACRO

    ScopedLedgerEntry(ScopeIdT scopeID, LedgerEntry const& entry);
    ScopedLedgerEntry(ScopeIdT scopeID, LedgerEntry&& entry);

  public:
    LedgerEntryScopeID<S> const mScopeID;

    // Must construct with a scope.
    ScopedLedgerEntry<S>() = delete;

    ScopedLedgerEntry(ScopedLedgerEntry<S> const& other);
    ScopedLedgerEntry(ScopedLedgerEntry<S>&& other);
    ScopedLedgerEntry<S>& operator=(ScopedLedgerEntry<S> const& other);
    ScopedLedgerEntry<S>& operator=(ScopedLedgerEntry<S>&& other);

    LedgerEntry const& read_in_scope(LedgerEntryScope<S> const& scope) const;
    LedgerEntry& modify_in_scope(LedgerEntryScope<S> const& scope);

    bool operator==(ScopedLedgerEntry const& other) const;
    bool operator<(ScopedLedgerEntry const& other) const;
};

template <StaticLedgerEntryScope S> class ScopedOptionalLedgerEntry
{
    static constexpr StaticLedgerEntryScope staticScope = S;
    using ScopeIdT = LedgerEntryScopeID<S>;
    using ScopeT = LedgerEntryScope<S>;
    std::optional<LedgerEntry> mEntry;

    friend class LedgerEntryScope<S>;
#define FRIEND_MACRO(SCOPE) \
    friend class LedgerEntryScope<StaticLedgerEntryScope::SCOPE>;
    FOREACH_STATIC_LEDGER_ENTRY_SCOPE(FRIEND_MACRO)
#undef FRIEND_MACRO

    ScopedOptionalLedgerEntry(ScopeIdT scopeID,
                              std::optional<LedgerEntry> const& entry);
    ScopedOptionalLedgerEntry(ScopeIdT scopeID,
                              std::optional<LedgerEntry>&& entry);

  public:
    LedgerEntryScopeID<S> const mScopeID;

    // Must construct with a scope.
    ScopedOptionalLedgerEntry<S>() = delete;

    ScopedOptionalLedgerEntry(ScopedOptionalLedgerEntry<S> const& other);
    ScopedOptionalLedgerEntry(ScopedOptionalLedgerEntry<S>&& other);
    ScopedOptionalLedgerEntry<S>&
    operator=(ScopedOptionalLedgerEntry<S> const& other);
    ScopedOptionalLedgerEntry<S>&
    operator=(ScopedOptionalLedgerEntry<S>&& other);

    ScopedOptionalLedgerEntry(ScopedLedgerEntry<S> const& other);
    ScopedOptionalLedgerEntry(ScopedLedgerEntry<S>&& other);

    std::optional<LedgerEntry> const&
    read_in_scope(LedgerEntryScope<S> const& scope) const;
    std::optional<LedgerEntry>&
    modify_in_scope(LedgerEntryScope<S> const& scope);

    bool operator==(ScopedOptionalLedgerEntry const& other) const;
    bool operator<(ScopedOptionalLedgerEntry const& other) const;
};

using GlobalLedgerEntry =
    ScopedLedgerEntry<StaticLedgerEntryScope::GLOBAL_PAR_APPLY_STATE>;
using ThreadLedgerEntry =
    ScopedLedgerEntry<StaticLedgerEntryScope::THREAD_PAR_APPLY_STATE>;
using TxLedgerEntry =
    ScopedLedgerEntry<StaticLedgerEntryScope::TX_PAR_APPLY_STATE>;

using GlobalOptionalLedgerEntry =
    ScopedOptionalLedgerEntry<StaticLedgerEntryScope::GLOBAL_PAR_APPLY_STATE>;
using ThreadOptionalLedgerEntry =
    ScopedOptionalLedgerEntry<StaticLedgerEntryScope::THREAD_PAR_APPLY_STATE>;
using TxOptionalLedgerEntry =
    ScopedOptionalLedgerEntry<StaticLedgerEntryScope::TX_PAR_APPLY_STATE>;

// Defines valid scope transitions as (DEST, SOURCE) pairs for compile-time
// enforcement.
#define FOR_EACH_VALID_SCOPE_ADOPTION(MACRO) \
    MACRO(GLOBAL_PAR_APPLY_STATE, THREAD_PAR_APPLY_STATE) \
    MACRO(THREAD_PAR_APPLY_STATE, GLOBAL_PAR_APPLY_STATE) \
    MACRO(THREAD_PAR_APPLY_STATE, TX_PAR_APPLY_STATE) \
    MACRO(TX_PAR_APPLY_STATE, THREAD_PAR_APPLY_STATE)

template <StaticLedgerEntryScope Dest, StaticLedgerEntryScope Source>
struct IsValidScopeAdoption : std::false_type
{
};

#define VALID_ADOPTION_SPECIALIZATION(DEST_SCOPE, SOURCE_SCOPE) \
    template <> \
    struct IsValidScopeAdoption<StaticLedgerEntryScope::DEST_SCOPE, \
                                StaticLedgerEntryScope::SOURCE_SCOPE> \
        : std::true_type \
    { \
    };

FOR_EACH_VALID_SCOPE_ADOPTION(VALID_ADOPTION_SPECIALIZATION)
#undef VALID_ADOPTION_SPECIALIZATION

template <StaticLedgerEntryScope S> class LedgerEntryScope
{
    mutable bool mActive{true};

#define FRIEND_MACRO(SCOPE) \
    friend class LedgerEntryScope<StaticLedgerEntryScope::SCOPE>;
    FOREACH_STATIC_LEDGER_ENTRY_SCOPE(FRIEND_MACRO)
#undef FRIEND_MACRO

  public:
    static constexpr StaticLedgerEntryScope staticScope = S;
    using ScopeIdT = LedgerEntryScopeID<S>;
    using EntryT = ScopedLedgerEntry<S>;
    using OptionalEntryT = ScopedOptionalLedgerEntry<S>;
    LedgerEntryScopeID<S> const mScopeID;

    LedgerEntryScope(ScopeIdT scopeID);

    // These two methods are marked const even though technically they modify
    // the mActive bit; this is to allow usage patterns where the scope is
    // logically immutable (e.g. a const ref to a scope passed into a function)
    // but we want to go a step further and dynamically deactivate _reading_
    // from it to prevent accidental access to stale data.
    void scope_activate() const;
    void scope_deactivate() const;

    LedgerEntry const& scope_read_entry(EntryT const& w) const;
    LedgerEntry& scope_modify_entry(EntryT& w) const;

    std::optional<LedgerEntry> const&
    scope_read_optional_entry(OptionalEntryT const& w) const;
    std::optional<LedgerEntry>&
    scope_modify_optional_entry(OptionalEntryT& w) const;

    EntryT scope_adopt_entry(LedgerEntry&& entry) const;
    EntryT scope_adopt_entry(LedgerEntry const& entry) const;
    OptionalEntryT
    scope_adopt_optional_entry(std::optional<LedgerEntry> const& entry) const;

    template <StaticLedgerEntryScope OtherScope>
    EntryT
    scope_adopt_entry_from(ScopedLedgerEntry<OtherScope> const& entry,
                           LedgerEntryScope<OtherScope> const& scope) const
    {
        static_assert(
            IsValidScopeAdoption<S, OtherScope>::value,
            "Invalid scope adoption: this transition is not allowed. "
            "Check FOR_EACH_VALID_SCOPE_ADOPTION in LedgerEntryScope.h "
            "for the list of valid transitions.");
        return scope_adopt_entry_from_impl(entry, scope);
    }

    template <StaticLedgerEntryScope OtherScope>
    OptionalEntryT
    scope_adopt_optional_entry_from(
        ScopedOptionalLedgerEntry<OtherScope> const& entry,
        LedgerEntryScope<OtherScope> const& scope) const
    {
        static_assert(
            IsValidScopeAdoption<S, OtherScope>::value,
            "Invalid scope adoption: this transition is not allowed. "
            "Check FOR_EACH_VALID_SCOPE_ADOPTION in LedgerEntryScope.h "
            "for the list of valid transitions.");
        return scope_adopt_optional_entry_from_impl(entry, scope);
    }

  private:
    template <StaticLedgerEntryScope OtherScope>
    EntryT scope_adopt_entry_from_impl(
        ScopedLedgerEntry<OtherScope> const& entry,
        LedgerEntryScope<OtherScope> const& scope) const;

    template <StaticLedgerEntryScope OtherScope>
    OptionalEntryT scope_adopt_optional_entry_from_impl(
        ScopedOptionalLedgerEntry<OtherScope> const& entry,
        LedgerEntryScope<OtherScope> const& scope) const;
};

template <StaticLedgerEntryScope S> class DeactivateScopeGuard
{
    LedgerEntryScope<S> const& mScope;

  public:
    DeactivateScopeGuard(LedgerEntryScope<S> const& scope);
    ~DeactivateScopeGuard();
};

#define DECLARE_EXTERN_TEMPLATES(SCOPE) \
    extern template class LedgerEntryScopeID<StaticLedgerEntryScope::SCOPE>; \
    extern template class LedgerEntryScope<StaticLedgerEntryScope::SCOPE>; \
    extern template class ScopedLedgerEntry<StaticLedgerEntryScope::SCOPE>; \
    extern template class ScopedOptionalLedgerEntry< \
        StaticLedgerEntryScope::SCOPE>; \
    extern template class DeactivateScopeGuard<StaticLedgerEntryScope::SCOPE>;
FOREACH_STATIC_LEDGER_ENTRY_SCOPE(DECLARE_EXTERN_TEMPLATES)

#undef DECLARE_EXTERN_TEMPLATES

}
