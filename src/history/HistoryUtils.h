// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <cstdint>
#include <functional>

namespace stellar
{

class XDRInputFileStream;

// This function centralizes the logic for iterating through
// history archive tx set entries (TransactionHistoryEntry,
// TransactionHistoryResultEntry) that may have gaps. Reads an entry from the
// stream into currentEntry until eof or an entry is found with ledgerSeq >=
// targetLedger. Returns true if targetLedger found (currentEntry will be set to
// the target ledger). Otherwise returns false, where currentEntry is the last
// entry read from the stream.
template <typename T>
bool getHistoryEntryForLedger(
    XDRInputFileStream& stream, T& currentEntry, uint32_t targetLedger,
    std::function<void(uint32_t ledgerSeq)> validateFn = nullptr);
}