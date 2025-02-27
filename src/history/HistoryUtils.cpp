// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "history/HistoryUtils.h"
#include "util/Logging.h"
#include "util/XDRStream.h"
#include "xdr/Stellar-ledger.h"
#include <Tracy.hpp>

namespace stellar
{

template <typename T>
bool
getHistoryEntryForLedger(XDRInputFileStream& stream, T& currentEntry,
                         uint32_t targetLedger,
                         std::function<void(uint32_t ledgerSeq)> validateFn)
{
    ZoneScoped;

    auto readNextWithValidation = [&]() {
        auto res = stream.readOne(currentEntry);
        if (res && validateFn)
        {
            validateFn(currentEntry.ledgerSeq);
        }
        return res;
    };

    do
    {
        if (currentEntry.ledgerSeq < targetLedger)
        {
            CLOG_DEBUG(History, "Advancing past txhistory entry for ledger {}",
                       currentEntry.ledgerSeq);
        }
        else if (currentEntry.ledgerSeq > targetLedger)
        {
            // No entry for this ledger
            break;
        }
        else
        {
            // Found the entry for our target ledger
            return true;
        }
    } while (stream && readNextWithValidation());

    return false;
}

template bool getHistoryEntryForLedger<TransactionHistoryEntry>(
    XDRInputFileStream& stream, TransactionHistoryEntry& currentEntry,
    uint32_t targetLedger, std::function<void(uint32_t ledgerSeq)> validateFn);

template bool getHistoryEntryForLedger<TransactionHistoryResultEntry>(
    XDRInputFileStream& stream, TransactionHistoryResultEntry& currentEntry,
    uint32_t targetLedger, std::function<void(uint32_t ledgerSeq)> validateFn);
}