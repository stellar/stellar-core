// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "catchup/simulation/HistoryArchiveStream.h"
#include "work/Work.h"
#include "xdr/Stellar-ledger.h"

namespace stellar
{
struct LedgerRange;

class ApplyTransactionsWork : public BasicWork
{
    TmpDir const& mDownloadDir;
    LedgerRange const mRange;
    Hash const mNetworkID;

    std::unique_ptr<HistoryArchiveStream> mStream;
    LedgerHeaderHistoryEntry mHeaderHistory;
    TransactionHistoryEntry mTransactionHistory;
    std::vector<TransactionEnvelope>::const_iterator mTransactionIter;
    TransactionHistoryResultEntry mResultHistory;
    std::vector<TransactionResultPair>::const_iterator mResultIter;

    uint32_t const mMaxOperations;

    bool getNextLedgerFromHistoryArchive();

    bool getNextLedger(std::vector<TransactionEnvelope>& transactions,
                       std::vector<TransactionResultPair>& results,
                       std::vector<UpgradeType>& upgrades);

  public:
    ApplyTransactionsWork(Application& app, TmpDir const& downloadDir,
                          LedgerRange const& range,
                          std::string const& networkPassphrase,
                          uint32_t desiredOperations);

  protected:
    void onReset() override;

    State onRun() override;

    bool onAbort() override;
};
}
