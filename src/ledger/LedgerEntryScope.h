#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "xdr/Stellar-ledger-entries.h"
#include <cstdint>
#include <iosfwd>
#include <optional>

// Defines the static set of scopes. Each of these turns into a static enum
// entry in StaticLedgerEntryScope as well as a bunch of type synonyms for
// scoped ledger entries parameterized by the enums.

#define FOREACH_STATIC_LEDGER_ENTRY_SCOPE(MACRO) \
    MACRO(GlobalParApply) \
    MACRO(ThreadParApply) \
    MACRO(TxParApply) \
    MACRO(LclSnapshot) \
    MACRO(HotArchive) \
    MACRO(RawBucket)

// Defines valid scope transitions as (DEST, SOURCE) pairs for compile-time
// enforcement. The "adopt" methods only allow ScopedLedgerEntries to
// be adopted between pairs of scopes listed here.
#define FOR_EACH_VALID_SCOPE_ADOPTION(MACRO) \
    MACRO(GlobalParApply, ThreadParApply) \
    MACRO(ThreadParApply, GlobalParApply) \
    MACRO(ThreadParApply, TxParApply) \
    MACRO(TxParApply, ThreadParApply)

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
    sizeof(LedgerEntryScopeID<StaticLedgerEntryScope::GlobalParApply>) == 8,
    "Unexpected size for LedgerEntryScopeID");

template <StaticLedgerEntryScope T> class LedgerEntryScope;
template <StaticLedgerEntryScope S> class ScopedLedgerEntryOpt;

template <StaticLedgerEntryScope S> class ScopedLedgerEntry
{
    static constexpr StaticLedgerEntryScope staticScope = S;
    using ScopeIdT = LedgerEntryScopeID<S>;
    using ScopeT = LedgerEntryScope<S>;
    LedgerEntry mEntry;

    friend class LedgerEntryScope<S>;
    friend class ScopedLedgerEntryOpt<S>;
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

template <StaticLedgerEntryScope S> class ScopedLedgerEntryOpt
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

    ScopedLedgerEntryOpt(ScopeIdT scopeID,
                         std::optional<LedgerEntry> const& entry);
    ScopedLedgerEntryOpt(ScopeIdT scopeID, std::optional<LedgerEntry>&& entry);

  public:
    LedgerEntryScopeID<S> const mScopeID;

    // Must construct with a scope.
    ScopedLedgerEntryOpt<S>() = delete;

    ScopedLedgerEntryOpt(ScopedLedgerEntryOpt<S> const& other);
    ScopedLedgerEntryOpt(ScopedLedgerEntryOpt<S>&& other);
    ScopedLedgerEntryOpt<S>& operator=(ScopedLedgerEntryOpt<S> const& other);
    ScopedLedgerEntryOpt<S>& operator=(ScopedLedgerEntryOpt<S>&& other);

    ScopedLedgerEntryOpt(ScopedLedgerEntry<S> const& other);
    ScopedLedgerEntryOpt(ScopedLedgerEntry<S>&& other);

    std::optional<LedgerEntry> const&
    read_in_scope(LedgerEntryScope<S> const& scope) const;
    std::optional<LedgerEntry>&
    modify_in_scope(LedgerEntryScope<S> const& scope);

    bool operator==(ScopedLedgerEntryOpt const& other) const;
    bool operator<(ScopedLedgerEntryOpt const& other) const;
};

#define SCOPE_ALIAS(SCOPE) \
    using SCOPE##LedgerEntry = \
        ScopedLedgerEntry<StaticLedgerEntryScope::SCOPE>; \
    using SCOPE##LedgerEntryOpt = \
        ScopedLedgerEntryOpt<StaticLedgerEntryScope::SCOPE>;
FOREACH_STATIC_LEDGER_ENTRY_SCOPE(SCOPE_ALIAS)
#undef SCOPE_ALIAS

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
    using OptionalEntryT = ScopedLedgerEntryOpt<S>;
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
        ScopedLedgerEntryOpt<OtherScope> const& entry,
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
        ScopedLedgerEntryOpt<OtherScope> const& entry,
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
    extern template class ScopedLedgerEntryOpt<StaticLedgerEntryScope::SCOPE>; \
    extern template class DeactivateScopeGuard<StaticLedgerEntryScope::SCOPE>;
FOREACH_STATIC_LEDGER_ENTRY_SCOPE(DECLARE_EXTERN_TEMPLATES)

#undef DECLARE_EXTERN_TEMPLATES

}
