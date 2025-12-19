// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerEntryScope.h"
#include "ledger/LedgerHashUtils.h"
#include "util/types.h"
#include "xdr/Stellar-ledger-entries.h"
#include <cstddef>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <stdexcept>

/////////////////////////////////////
// LedgerEntryScopeID
/////////////////////////////////////

template <stellar::StaticLedgerEntryScope S>
std::ostream&
operator<<(std::ostream& os, stellar::LedgerEntryScopeID<S> const& obj)
{
    switch (S)
    {
#define STATIC_SCOPE_MACRO(SCOPE_NAME) \
    case stellar::StaticLedgerEntryScope::SCOPE_NAME: \
        os << #SCOPE_NAME; \
        break;
        FOREACH_STATIC_LEDGER_ENTRY_SCOPE(STATIC_SCOPE_MACRO)
#undef STATIC_SCOPE_MACRO
    }
    os << "[" << obj.mIndex << "]";
    if (obj.mLedger != 0)
        os << " @ Ledger " << obj.mLedger;
    return os;
}

namespace fmt
{
template <stellar::StaticLedgerEntryScope S>
struct formatter<stellar::LedgerEntryScopeID<S>> : ostream_formatter
{
};
}

namespace stellar
{

uint16_t
clamp16(size_t v)
{
    if (v > 0xFFFF)
    {
        throw std::runtime_error(
            fmt::format("clamp16: value {} exceeds 16 bit limit", v));
    }
    return static_cast<uint16_t>(v);
}

template <StaticLedgerEntryScope S>
LedgerEntryScopeID<S>::LedgerEntryScopeID(size_t index, uint32_t ledger)
    : mIndex(clamp16(index)), mLedger(ledger)
{
}

template <StaticLedgerEntryScope S>
bool
LedgerEntryScopeID<S>::operator==(LedgerEntryScopeID<S> const& other) const
{
    return mLedger == other.mLedger && mIndex == other.mIndex;
}

template <StaticLedgerEntryScope S>
bool
LedgerEntryScopeID<S>::operator!=(LedgerEntryScopeID<S> const& other) const
{
    return !(*this == other);
}

/////////////////////////////////////
// ScopedLedgerEntry
/////////////////////////////////////

template <StaticLedgerEntryScope S>
ScopedLedgerEntry<S>::ScopedLedgerEntry(ScopeIdT scopeID,
                                        LedgerEntry const& entry)
    : mEntry(entry), mScopeID(scopeID)
{
}

template <StaticLedgerEntryScope S>
ScopedLedgerEntry<S>::ScopedLedgerEntry(ScopeIdT scopeID, LedgerEntry&& entry)
    : mEntry(std::move(entry)), mScopeID(scopeID)
{
}

template <StaticLedgerEntryScope S>
ScopedLedgerEntry<S>::ScopedLedgerEntry(ScopedLedgerEntry<S> const& other)
    : mEntry(other.mEntry), mScopeID(other.mScopeID)
{
}

template <StaticLedgerEntryScope S>
ScopedLedgerEntry<S>::ScopedLedgerEntry(ScopedLedgerEntry<S>&& other)
    : mEntry(std::move(other.mEntry)), mScopeID(other.mScopeID)
{
}

template <StaticLedgerEntryScope S>
ScopedLedgerEntry<S>&
ScopedLedgerEntry<S>::operator=(ScopedLedgerEntry<S> const& other)
{
    if (this == &other)
    {
        return *this;
    }
    if (other.mScopeID != mScopeID)
    {
        throw std::runtime_error(
            fmt::format("operator=: scope ID '{}' != entry scope ID '{}'",
                        mScopeID, other.mScopeID));
    }
    mEntry = other.mEntry;
    return *this;
}

template <StaticLedgerEntryScope S>
ScopedLedgerEntry<S>&
ScopedLedgerEntry<S>::operator=(ScopedLedgerEntry<S>&& other)
{
    if (this == &other)
    {
        return *this;
    }
    if (other.mScopeID != mScopeID)
    {
        throw std::runtime_error(
            fmt::format("move operator=: scope ID '{}' != entry scope ID '{}'",
                        mScopeID, other.mScopeID));
    }
    mEntry = std::move(other.mEntry);
    return *this;
}

template <StaticLedgerEntryScope S>
LedgerEntry const&
ScopedLedgerEntry<S>::read_in_scope(LedgerEntryScope<S> const& scope) const
{
    return scope.scope_read_entry(*this);
}

template <StaticLedgerEntryScope S>
void
ScopedLedgerEntry<S>::modify_in_scope(LedgerEntryScope<S> const& scope,
                                      std::function<void(LedgerEntry&)> func)
{
    scope.scope_modify_entry(*this, func);
}

template <StaticLedgerEntryScope S>
bool
ScopedLedgerEntry<S>::operator==(ScopedLedgerEntry<S> const& other) const
{
    if (mScopeID != other.mScopeID)
    {
        throw std::runtime_error(
            fmt::format("operator==: scope ID '{}' != other scope ID '{}'",
                        mScopeID, other.mScopeID));
    }
    return mEntry == other.mEntry;
}

template <StaticLedgerEntryScope S>
bool
ScopedLedgerEntry<S>::operator<(ScopedLedgerEntry<S> const& other) const
{
    if (mScopeID != other.mScopeID)
    {
        throw std::runtime_error(
            fmt::format("operator<: scope ID '{}' != other scope ID '{}'",
                        mScopeID, other.mScopeID));
    }
    return mEntry < other.mEntry;
}

/////////////////////////////////////
// ScopedLedgerEntryOpt
/////////////////////////////////////

template <StaticLedgerEntryScope S>
ScopedLedgerEntryOpt<S>::ScopedLedgerEntryOpt(
    ScopeIdT scopeID, std::optional<LedgerEntry> const& entry)
    : mEntry(entry), mScopeID(scopeID)
{
}

template <StaticLedgerEntryScope S>
ScopedLedgerEntryOpt<S>::ScopedLedgerEntryOpt(
    ScopeIdT scopeID, std::optional<LedgerEntry>&& entry)
    : mEntry(std::move(entry)), mScopeID(scopeID)
{
}

template <StaticLedgerEntryScope S>
ScopedLedgerEntryOpt<S>::ScopedLedgerEntryOpt(
    ScopedLedgerEntryOpt<S> const& other)
    : mEntry(other.mEntry), mScopeID(other.mScopeID)
{
}

template <StaticLedgerEntryScope S>
ScopedLedgerEntryOpt<S>::ScopedLedgerEntryOpt(ScopedLedgerEntryOpt<S>&& other)
    : mEntry(std::move(other.mEntry)), mScopeID(other.mScopeID)
{
}

template <StaticLedgerEntryScope S>
ScopedLedgerEntryOpt<S>::ScopedLedgerEntryOpt(ScopedLedgerEntry<S> const& other)
    : mEntry(other.mEntry), mScopeID(other.mScopeID)
{
}

template <StaticLedgerEntryScope S>
ScopedLedgerEntryOpt<S>::ScopedLedgerEntryOpt(ScopedLedgerEntry<S>&& other)
    : mEntry(std::move(other.mEntry)), mScopeID(other.mScopeID)
{
}

template <StaticLedgerEntryScope S>
ScopedLedgerEntryOpt<S>&
ScopedLedgerEntryOpt<S>::operator=(ScopedLedgerEntryOpt<S> const& other)
{
    if (this == &other)
    {
        return *this;
    }
    if (other.mScopeID != mScopeID)
    {
        throw std::runtime_error(
            fmt::format("operator=: scope ID '{}' != entry scope ID '{}'",
                        mScopeID, other.mScopeID));
    }
    mEntry = other.mEntry;
    return *this;
}

template <StaticLedgerEntryScope S>
ScopedLedgerEntryOpt<S>&
ScopedLedgerEntryOpt<S>::operator=(ScopedLedgerEntryOpt<S>&& other)
{
    if (this == &other)
    {
        return *this;
    }
    if (other.mScopeID != mScopeID)
    {
        throw std::runtime_error(
            fmt::format("move operator=: scope ID '{}' != entry scope ID '{}'",
                        mScopeID, other.mScopeID));
    }
    mEntry = std::move(other.mEntry);
    return *this;
}

template <StaticLedgerEntryScope S>
std::optional<LedgerEntry> const&
ScopedLedgerEntryOpt<S>::read_in_scope(LedgerEntryScope<S> const& scope) const
{
    return scope.scope_read_optional_entry(*this);
}

template <StaticLedgerEntryScope S>
void
ScopedLedgerEntryOpt<S>::modify_in_scope(
    LedgerEntryScope<S> const& scope,
    std::function<void(std::optional<LedgerEntry>&)> func)
{
    scope.scope_modify_optional_entry(*this, func);
}

template <StaticLedgerEntryScope S>
bool
ScopedLedgerEntryOpt<S>::operator==(ScopedLedgerEntryOpt<S> const& other) const
{
    if (mScopeID != other.mScopeID)
    {
        throw std::runtime_error(
            fmt::format("operator==: scope ID '{}' != other scope ID '{}'",
                        mScopeID, other.mScopeID));
    }
    return mEntry == other.mEntry;
}

template <StaticLedgerEntryScope S>
bool
ScopedLedgerEntryOpt<S>::operator<(ScopedLedgerEntryOpt<S> const& other) const
{
    if (mScopeID != other.mScopeID)
    {
        throw std::runtime_error(
            fmt::format("operator<: scope ID '{}' != other scope ID '{}'",
                        mScopeID, other.mScopeID));
    }
    return mEntry < other.mEntry;
}

/////////////////////////////////////
// LedgerEntryScope
/////////////////////////////////////

template <StaticLedgerEntryScope S>
LedgerEntryScope<S>::LedgerEntryScope(LedgerEntryScopeID<S> scopeID)
    : mScopeID(scopeID)
{
}

template <StaticLedgerEntryScope S>
void
LedgerEntryScope<S>::scope_activate() const
{
    if (mActive)
    {
        throw std::runtime_error(fmt::format(
            "LedgerEntryScope::scope_activate: scope {} already active",
            mScopeID));
    }
    mActive = true;
}

template <StaticLedgerEntryScope S>
void
LedgerEntryScope<S>::scope_deactivate() const
{
    if (!mActive)
    {
        throw std::runtime_error(fmt::format(
            "LedgerEntryScope::scope_deactivate: scope {} already inactive",
            mScopeID));
    }
    mActive = false;
}

template <StaticLedgerEntryScope S>
LedgerEntry const&
LedgerEntryScope<S>::scope_read_entry(ScopedLedgerEntry<S> const& w) const
{
    if (w.mScopeID != mScopeID)
    {
        throw std::runtime_error(fmt::format(
            "scope_read_entry: scope ID '{}' != entry scope ID '{}'", mScopeID,
            w.mScopeID));
    }
    return w.mEntry;
}

template <StaticLedgerEntryScope S>
void
LedgerEntryScope<S>::scope_modify_entry(
    ScopedLedgerEntry<S>& w, std::function<void(LedgerEntry&)> func) const
{
    if (w.mScopeID != mScopeID)
    {
        throw std::runtime_error(fmt::format(
            "scope_modify_entry: scope ID '{}' != entry scope ID '{}'",
            mScopeID, w.mScopeID));
    }
    func(w.mEntry);
}

template <StaticLedgerEntryScope S>
std::optional<LedgerEntry> const&
LedgerEntryScope<S>::scope_read_optional_entry(
    ScopedLedgerEntryOpt<S> const& w) const
{
    if (w.mScopeID != mScopeID)
    {
        throw std::runtime_error(fmt::format(
            "scope_read_optional_entry: scope ID '{}' != entry scope ID '{}'",
            mScopeID, w.mScopeID));
    }
    return w.mEntry;
}

template <StaticLedgerEntryScope S>
void
LedgerEntryScope<S>::scope_modify_optional_entry(
    ScopedLedgerEntryOpt<S>& w,
    std::function<void(std::optional<LedgerEntry>&)> func) const
{
    if (w.mScopeID != mScopeID)
    {
        throw std::runtime_error(fmt::format(
            "scope_modify_optional_entry: scope ID '{}' != entry scope ID '{}'",
            mScopeID, w.mScopeID));
    }
    func(w.mEntry);
}

template <StaticLedgerEntryScope S>
ScopedLedgerEntry<S>
LedgerEntryScope<S>::scope_adopt_entry(LedgerEntry&& entry) const
{
    return ScopedLedgerEntry(mScopeID, std::move(entry));
}

template <StaticLedgerEntryScope S>
ScopedLedgerEntry<S>
LedgerEntryScope<S>::scope_adopt_entry(LedgerEntry const& entry) const
{
    return ScopedLedgerEntry(mScopeID, entry);
}

template <StaticLedgerEntryScope S>
ScopedLedgerEntryOpt<S>
LedgerEntryScope<S>::scope_adopt_optional_entry(
    std::optional<LedgerEntry> const& entry) const
{
    return ScopedLedgerEntryOpt(mScopeID, entry);
}

template <StaticLedgerEntryScope S>
template <StaticLedgerEntryScope OtherScope>
ScopedLedgerEntry<S>
LedgerEntryScope<S>::scope_adopt_entry_from_impl(
    ScopedLedgerEntry<OtherScope> const& entry,
    LedgerEntryScope<OtherScope> const& scope) const
{
    // NB: Here we do _not_ do a `scope_read_entry` on the adopting-from scope.
    // Quite the opposite! We check that the adopting-from scope is _inactive_
    // so that it's correct for us to be adopting the entry without introducing
    // risk of read inconsistency.
    if (scope.mActive)
    {
        throw std::runtime_error(fmt::format(
            "scope_adopt_entry_from: adopting entry with scope ID {} from "
            "still-active scope ID '{}'",
            entry.mScopeID, scope.mScopeID));
    }
    return EntryT{mScopeID, entry.mEntry};
}

template <StaticLedgerEntryScope S>
template <StaticLedgerEntryScope OtherScope>
ScopedLedgerEntryOpt<S>
LedgerEntryScope<S>::scope_adopt_optional_entry_from_impl(
    ScopedLedgerEntryOpt<OtherScope> const& entry,
    LedgerEntryScope<OtherScope> const& scope) const
{
    if (scope.mActive)
    {
        throw std::runtime_error(
            fmt::format("scope_adopt_optional_entry_from: adopting entry with "
                        "scope ID {} from "
                        "still-active scope ID '{}'",
                        entry.mScopeID, scope.mScopeID));
    }
    return ScopedLedgerEntryOpt<S>{mScopeID, entry.mEntry};
}

/////////////////////////////////
// DeactivateScopeGuard
/////////////////////////////////

template <StaticLedgerEntryScope S>
DeactivateScopeGuard<S>::DeactivateScopeGuard(LedgerEntryScope<S> const& scope)
    : mScope(scope)
{
    mScope.scope_deactivate();
}
template <StaticLedgerEntryScope S>
DeactivateScopeGuard<S>::~DeactivateScopeGuard()
{
    mScope.scope_activate();
}

#define INSTANTIATE_SCOPE_CLASSES(SCOPE_NAME) \
    template class LedgerEntryScopeID<StaticLedgerEntryScope::SCOPE_NAME>; \
    template class LedgerEntryScope<StaticLedgerEntryScope::SCOPE_NAME>; \
    template class ScopedLedgerEntry<StaticLedgerEntryScope::SCOPE_NAME>; \
    template class ScopedLedgerEntryOpt<StaticLedgerEntryScope::SCOPE_NAME>; \
    template class DeactivateScopeGuard<StaticLedgerEntryScope::SCOPE_NAME>;

FOREACH_STATIC_LEDGER_ENTRY_SCOPE(INSTANTIATE_SCOPE_CLASSES)
#undef INSTANTIATE_SCOPE_CLASSES

#define INSTANTIATE_ADOPT_METHODS(DEST_SCOPE, SOURCE_SCOPE) \
    template ScopedLedgerEntry<StaticLedgerEntryScope::DEST_SCOPE> \
    LedgerEntryScope<StaticLedgerEntryScope::DEST_SCOPE>:: \
        scope_adopt_entry_from_impl<StaticLedgerEntryScope::SOURCE_SCOPE>( \
            ScopedLedgerEntry<StaticLedgerEntryScope::SOURCE_SCOPE> const&, \
            LedgerEntryScope<StaticLedgerEntryScope::SOURCE_SCOPE> const&) \
            const; \
\
    template ScopedLedgerEntryOpt<StaticLedgerEntryScope::DEST_SCOPE> \
    LedgerEntryScope<StaticLedgerEntryScope::DEST_SCOPE>:: \
        scope_adopt_optional_entry_from_impl< \
            StaticLedgerEntryScope::SOURCE_SCOPE>( \
            ScopedLedgerEntryOpt<StaticLedgerEntryScope::SOURCE_SCOPE> const&, \
            LedgerEntryScope<StaticLedgerEntryScope::SOURCE_SCOPE> const&) \
            const;

FOR_EACH_VALID_SCOPE_ADOPTION(INSTANTIATE_ADOPT_METHODS)
#undef INSTANTIATE_ADOPT_METHODS

}
