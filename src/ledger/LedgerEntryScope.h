#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "xdr/Stellar-ledger-entries.h"
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <optional>

// Overview:
// ~~~~~~~~~
//
// This file defines ScopedLedgerEntry and ScopedLedgerEntryOpt which
// are wrappers around LedgerEntry and std::optional<LedgerEntry>, as
// well as a number of LedgerEntryScope<S> mixin-classes that such
// ScopedLedgerEntries can belong to.
//
// The purpose of these is to catch _unintentional misuses_ of
// LedgerEntries in a number of common ways:
//
//    - Ledger skew: using an LE state in ledger X in ledger X+1.
//
//    - Thread races: using an LE partitioned to thread X in thread Y.
//
//    - Stale reads: reading an LE from an outer ledger scope when
//      there's an active inner tx-local LE that should supercede it.
//
//    - Lost writes: writing an LE to an outer ledger scope when
//      there's an active inner tx-local LE that will overwrite it
//      when the inner scope commits its changes.
//
// Some of this was formerly handled by the machinery of the LedgerTxn
// classes but these have a fairly brittle structure and are not
// threadsafe, so are not being used as much in new code (eg. parallel
// ledger apply, bucketlist eviction, etc.)
//
//
// Scope IDs:
// ~~~~~~~~~~
//
// Each ScopedLedgerEntry (SLE) and each LedgerEntryScope is assigned
// a LedgerEntryScopeID.
//
// A LedgerEntryScopeID contains both a static component and a dynamic
// component:
//
//     - The static component is an enum class called
//       StaticLedgerEntryScope (generated from the macro
//       FOREACH_STATIC_LEDGER_ENTRY_SCOPE) which has one enum variant
//       for each broad static context in the program that deals with
//       LedgerEntries (global-parallel-apply, tx-parallel-apply, LCL
//       snapshot, etc.) This enum class is provided as a constant
//       template parameter to all the other classes in this file --
//       SLEs and scopes -- such that misusing an SLE from scope X in
//       scope Y will not even compile.
//
//     - The dynamic component is a pair of numbers, one of which is
//       always the ledger and the other of which is left as a generic
//       "index" number with per-scope meaning (for example the
//       "index" is the parallel-apply thread-number in the scope used
//       for parallel-apply threads). These numbers are checked
//       _dynamically_ so misusing an SLE from scope X in scope Y will
//       still compile, but will fail at runtime.
//
//
// Scope classes:
// ~~~~~~~~~~~~~~
//
// Each _conceptual_ scope (identified by some scope S taken from the
// StaticLedgerEntryScope enum) typically has one or more _specific
// classes_ in the program that hold, track, or otherwise work-with
// LedgerEntries (usually in hashmaps). These specific classes should
// all inherit from the mixin-class LedgerEntryScope<S>, which will
// provide them with 3 things:
//
//     - A field member `LedgerEntryScopeID<S> mScopeID` that will
//       need to be initialized with dynamic values for its dynamic
//       components at class-construction time.
//
//     - The ability to "activate" or "deactivate" the scope. See
//       below.
//
//     - Methods for reading, writing and moving SLEs associated with
//       that scope. these methods all begin with `scope` and are
//       the main interface for connecting SLEs to scopes. See below.
//
//
//
// Scope activation:
// ~~~~~~~~~~~~~~~~~
//
// In addition to having a "scope ID", each scope has a boolean
// "activation status". When active, SLEs can be read and written
// using the scope's read/write methods. When inactive, SLEs
// cannot be read/written, only adopted (moved) to other scopes.
//
// The point of "activation" is to allow _deactivation_ of a
// conceptual "outer" scope when some conceptual "inner" scope is
// active, to prevent stale reads from (or lost writes to) the
// outer scope until the inner scope completes.
//
// Since stellar-core uses exceptions in more than a few places,
// you should use the helper DeactivateScopeGuard class which
// deactivates an outer scope on construction and re-activates
// it on destruction.
//
//
// Reading and writing LedgerEntries from ScopedLedgerEntries:
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// When you sctually want to _use_ an SLE (i.e. read or write the LE
// it is wrapped around, or any fields inside the LE) you must
// _provide the scope_ that you want to read or write with, using the
// methods `scopeReadEntry` or `scopeModifyEntry`. The scope will
// be checked (both statically and dynamically) for compatibility with
// the SLE.
//
// The SLEs themselves have methods on them that might make this
// slightly easier to read -- `readInScope` and `modifyInScope` --
// but they call through to the same methods on the scope.
//
//
// Creating and moving ScopedLedgerEntries:
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Of course LEs do frequently have to _move_ between scopes! The
// point of all this machinery is not to fundamentally prevent you
// from doing so, only to catch _accidental_ uses across scope
// boundaries.
//
// To turn an LE into an SLE in a given scope, call `scopeAdoptEntry`.
//
// To move an SLE from one scope to another, call `scopeAdoptEntryFrom`.
//
// There is a strict "allow list" (defined by the macro
// FOR_EACH_VALID_SCOPE_ADOPTION) of scope-to-scope movements that are
// allowed. Again, this is just to catch misuses: if you have a
// legitimate reason to move an SLE from one scope to another that's
// not in the list, you can add it below.
//
//
// Optional SLEs:
// ~~~~~~~~~~~~~~
//
// All of the above is duplicated for _optional_ LEs and SLEs. That
// is: rather than use std::optional<ScopedLedgerEntry<S>>, there is a
// special class ScopedLedgerEntryOpt<S>. The reason for this to exist
// is that we want the scope checking to happen _even when_ we are
// reading/writing/moving `nullopt` values around. The
// ScopedLedgerEntryOpt type equips all optional LEs -- even nullopts
// -- with scope tracking, whereas an std::optional<SLE> would not.
//
//
// Type aliases:
// ~~~~~~~~~~~~~
//
// To make things easier, this file also defines a number of type
// aliases for each of the scopes. So rather than writing:
//
//     ScopedLedgerEntry<StaticLedgerEntryScope::GlobalParApply>
//
// one can write:
//
//     GlobalParApplyLedgerEntry
//
// and so forth. The alias list is defined by SCOPE_ALIAS.

// This macro defines the _static_ set of scopes. Each of these turns
// into a static enum entry in StaticLedgerEntryScope as well as a
// bunch of type synonyms for scoped ledger entries parameterized by
// the enums.

#define FOREACH_STATIC_LEDGER_ENTRY_SCOPE(MACRO) \
    MACRO(GlobalParApply) \
    MACRO(ThreadParApply) \
    MACRO(TxParApply) \
    MACRO(LclSnapshot) \
    MACRO(HotArchive) \
    MACRO(RawBucket)

// This macro defines valid scope transitions as (DEST, SOURCE) pairs
// for compile-time enforcement. The "adopt" methods only allow
// ScopedLedgerEntries to be adopted between pairs of scopes listed
// here.
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
    ScopedLedgerEntry() = delete;

    ScopedLedgerEntry(ScopedLedgerEntry<S> const& other);
    ScopedLedgerEntry(ScopedLedgerEntry<S>&& other);
    ScopedLedgerEntry<S>& operator=(ScopedLedgerEntry<S> const& other);
    ScopedLedgerEntry<S>& operator=(ScopedLedgerEntry<S>&& other);

    LedgerEntry const& readInScope(LedgerEntryScope<S> const& scope) const;
    void modifyInScope(LedgerEntryScope<S> const& scope,
                       std::function<void(LedgerEntry&)> func);

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
    ScopedLedgerEntryOpt() = delete;

    ScopedLedgerEntryOpt(ScopedLedgerEntryOpt<S> const& other);
    ScopedLedgerEntryOpt(ScopedLedgerEntryOpt<S>&& other);
    ScopedLedgerEntryOpt<S>& operator=(ScopedLedgerEntryOpt<S> const& other);
    ScopedLedgerEntryOpt<S>& operator=(ScopedLedgerEntryOpt<S>&& other);

    ScopedLedgerEntryOpt(ScopedLedgerEntry<S> const& other);
    ScopedLedgerEntryOpt(ScopedLedgerEntry<S>&& other);

    std::optional<LedgerEntry> const&
    readInScope(LedgerEntryScope<S> const& scope) const;
    void modifyInScope(LedgerEntryScope<S> const& scope,
                       std::function<void(std::optional<LedgerEntry>&)> func);

    bool operator==(ScopedLedgerEntryOpt const& other) const;
    bool operator<(ScopedLedgerEntryOpt const& other) const;
};

// These are just type synonyms for different ScopedLedgerEntry<S>
// and ScopedLedgerEntryOpt<S> specializations. For example
// GlobalParApplyLedgerEntry is just an abbreviation for
// ScopedLedgerEntry<StaticLedgerEntryScope::GlobalParApply>.
//
// The full list of such specialization is driven by the macro
// above: FOREACH_STATIC_LEDGER_ENTRY_SCOPE.

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
    void scopeActivate() const;
    void scopeDeactivate() const;

    LedgerEntry const& scopeReadEntry(EntryT const& w) const;
    void scopeModifyEntry(EntryT& w,
                          std::function<void(LedgerEntry&)> func) const;

    std::optional<LedgerEntry> const&
    scopeReadOptionalEntry(OptionalEntryT const& w) const;
    void scopeModifyOptionalEntry(
        OptionalEntryT& w,
        std::function<void(std::optional<LedgerEntry>&)> func) const;

    EntryT scopeAdoptEntry(LedgerEntry&& entry) const;
    EntryT scopeAdoptEntry(LedgerEntry const& entry) const;
    OptionalEntryT
    scopeAdoptEntryOpt(std::optional<LedgerEntry> const& entry) const;

    template <StaticLedgerEntryScope OtherScope>
    EntryT
    scopeAdoptEntryFrom(ScopedLedgerEntry<OtherScope> const& entry,
                        LedgerEntryScope<OtherScope> const& scope) const
    {
        static_assert(
            IsValidScopeAdoption<S, OtherScope>::value,
            "Invalid scope adoption: this transition is not allowed. "
            "Check FOR_EACH_VALID_SCOPE_ADOPTION in LedgerEntryScope.h "
            "for the list of valid transitions.");
        return scopeAdoptEntryFromImpl(entry, scope);
    }

    template <StaticLedgerEntryScope OtherScope>
    OptionalEntryT
    scopeAdoptEntryOptFrom(ScopedLedgerEntryOpt<OtherScope> const& entry,
                           LedgerEntryScope<OtherScope> const& scope) const
    {
        static_assert(
            IsValidScopeAdoption<S, OtherScope>::value,
            "Invalid scope adoption: this transition is not allowed. "
            "Check FOR_EACH_VALID_SCOPE_ADOPTION in LedgerEntryScope.h "
            "for the list of valid transitions.");
        return scopeAdoptEntryOptFromImpl(entry, scope);
    }

  private:
    template <StaticLedgerEntryScope OtherScope>
    EntryT
    scopeAdoptEntryFromImpl(ScopedLedgerEntry<OtherScope> const& entry,
                            LedgerEntryScope<OtherScope> const& scope) const;

    template <StaticLedgerEntryScope OtherScope>
    OptionalEntryT
    scopeAdoptEntryOptFromImpl(ScopedLedgerEntryOpt<OtherScope> const& entry,
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
