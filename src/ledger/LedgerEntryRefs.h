// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "xdr/Stellar-ledger-entries.h"

#include <functional>
#include <span>
#include <vector>

namespace stellar
{

using LedgerEntryRef = std::reference_wrapper<LedgerEntry const>;
using LedgerKeyRef = std::reference_wrapper<LedgerKey const>;
using LedgerEntryRefVec = std::vector<LedgerEntryRef>;
using LedgerKeyRefVec = std::vector<LedgerKeyRef>;

// Non-owning views over ledger entries and keys.
//
// These are std::span with two deliberate modifications:
//
//  - No default constructor. The functions taking these are overloaded
//    against ones taking `std::vector<LedgerEntry> const&` (in tests only), and
//    a default-constructible span would make the existing `f({})` call sites
//    ambiguous.
//
//  - Construction from an rvalue vector is deleted, so a view can never be
//    built from a temporary that dies before it is read. Callers holding
//    entries by value must therefore keep the result of `toRefs` in a named
//    local for as long as they use the view.
struct LedgerEntryRefs : std::span<LedgerEntryRef const>
{
    LedgerEntryRefs(LedgerEntryRefVec const& entries)
        : std::span<LedgerEntryRef const>(entries)
    {
    }
    LedgerEntryRefs(LedgerEntryRefVec&&) = delete;
};

struct LedgerKeyRefs : std::span<LedgerKeyRef const>
{
    LedgerKeyRefs(LedgerKeyRefVec const& keys)
        : std::span<LedgerKeyRef const>(keys)
    {
    }
    LedgerKeyRefs(LedgerKeyRefVec&&) = delete;
};

#ifdef BUILD_TESTS
// Returns a vector of references to the entries in the input vector.
// This is a helper for test-only wrappers that accept ledger entries/keys by
// value for convenience, but need to pass them to functions that take
// reference-based views.
template <typename T>
std::vector<std::reference_wrapper<T const>>
toRefs(std::vector<T> const& values)
{
    std::vector<std::reference_wrapper<T const>> res;
    res.reserve(values.size());
    for (auto const& v : values)
    {
        res.push_back(v);
    }
    return res;
}
#endif
} // namespace stellar