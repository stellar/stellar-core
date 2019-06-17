// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "ledger/LedgerRange.h"
#include "util/XDRStream.h"
#include "xdr/Stellar-ledger.h"

namespace stellar
{

class FileTransferInfo;
class HistoryManager;
class TmpDir;

class HistoryArchiveStream
{
    TmpDir const& mDownloadDir;
    LedgerRange const mRange;
    XDRInputFileStream mHeaderStream;
    XDRInputFileStream mTransactionStream;
    XDRInputFileStream mResultStream;

    HistoryManager const& mHistoryManager;

    LedgerHeaderHistoryEntry mHeaderHistory;
    TransactionHistoryEntry mTransactionHistory;
    TransactionHistoryResultEntry mResultHistory;

    LedgerHeaderHistoryEntry readHeaderHistory();
    LedgerHeaderHistoryEntry readHeaderHistory(uint32_t ledgerSeq);

    TransactionHistoryEntry makeEmptyTransactionHistory();
    TransactionHistoryEntry readTransactionHistory();

    TransactionHistoryResultEntry makeEmptyResultHistory();
    TransactionHistoryResultEntry readResultHistory();

  public:
    HistoryArchiveStream(TmpDir const& downloadDir, LedgerRange const& range,
                         HistoryManager const& hm);

    bool getNextLedger(LedgerHeaderHistoryEntry& header,
                       TransactionHistoryEntry& transaction,
                       TransactionHistoryResultEntry& result);
};
}
