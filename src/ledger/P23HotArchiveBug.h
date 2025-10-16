#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <vector>

#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-ledger.h"

namespace stellar
{
class Application;
class AbstractLedgerTxn;

namespace p23_hot_archive_bug
{
// 'Hide' these constants away as we don't really want to access them directly
// outside the tests.
namespace internal
{
constexpr size_t P23_CORRUPTED_HOT_ARCHIVE_ENTRIES_COUNT = 478;
extern const std::array<std::string, P23_CORRUPTED_HOT_ARCHIVE_ENTRIES_COUNT>
    P23_CORRUPTED_HOT_ARCHIVE_ENTRIES;
extern const std::array<std::string, P23_CORRUPTED_HOT_ARCHIVE_ENTRIES_COUNT>
    P23_CORRUPTED_HOT_ARCHIVE_ENTRY_CORRECT_STATE;
} // namespace internal

std::vector<LedgerKey> getP23CorruptedHotArchiveKeys();

void addHotArchiveBatchWithP23HotArchiveFix(
    AbstractLedgerTxn& ltx, Application& app, LedgerHeader header,
    std::vector<LedgerEntry> const& archivedEntries,
    std::vector<LedgerKey> const& restoredEntries);
} // namespace p23_hot_archive_bug
} // namespace stellar
