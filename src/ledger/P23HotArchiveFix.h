#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <vector>

#include "util/types.h"
#include "xdr/Stellar-ledger.h"

namespace stellar
{
class Application;
class AbstractLedgerTxn;
class LedgerHeader;

constexpr size_t P23_ARCHIVED_ENTRIES_TO_FIX_COUNT = 478;
// Only expose these for testing
#ifdef BUILD_TESTS
extern const std::array<std::string, P23_ARCHIVED_ENTRIES_TO_FIX_COUNT>
    P23_CORRUPTED_ENTRIES;
extern const std::array<std::string, P23_ARCHIVED_ENTRIES_TO_FIX_COUNT>
    P23_CORRUPTED_ENTRY_FIXES;
#endif

void addHotArchiveBatchWithP23HotArchiveFix(
    AbstractLedgerTxn& ltx, Application& app, LedgerHeader header,
    std::vector<LedgerEntry> const& archivedEntries,
    std::vector<LedgerKey> const& restoredEntries);
} // namespace stellar
