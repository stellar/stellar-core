// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "herder/LedgerCloseData.h"
#include "herder/TxSetFrame.h"
#include "history/HistoryArchive.h"
#include "ledger/LedgerRange.h"
#include "util/XDRStream.h"
#include "work/ConditionalWork.h"
#include "work/Work.h"
#include "xdr/Stellar-SCP.h"
#include "xdr/Stellar-ledger.h"

namespace stellar
{

class TmpDir;
struct LedgerHeaderHistoryEntry;

/**
 * This class is responsible for applying transactions stored in files in the
 * temporary directory (downloadDir) to local the ledger. It requires two sets
 * of files - ledgers and transactions - in .xdr format. Transaction files are
 * used to read transactions that will be applied and ledger files are used to
 * check if ledger hashes are matching.
 *
 * It may also require a third set of files - transaction results - to use in
 * accelerated replay, where failed transactions are not applied and successful
 * transactions are applied without verifying their signatures.
 *
 * In each run it skips or applies transactions from one ledger. Skipping occurs
 * when the ledger to be applied is older than the LCL of the local ledger. At
 * LCL, boundary checks are made to confirm that the ledgers from the files knit
 * up with LCL. If everything is OK, an apply ledger operation is performed.
 * Then another check is made - if the new local ledger matches corresponding
 * the ledger from file.
 *
 * The constructor of this class takes some important parameters:
 * * downloadDir - directory containing ledger and transaction files
 * * range - LedgerRange to apply, must be checkpoint-aligned,
 * and cover at most one checkpoint.
 */

class ApplyCheckpointWork : public BasicWork
{
    TmpDir const& mDownloadDir;
    LedgerRange const mLedgerRange;
    uint32_t const mCheckpoint;

    XDRInputFileStream mHdrIn;
    XDRInputFileStream mTxIn;
    TransactionHistoryEntry mTxHistoryEntry;
#ifdef BUILD_TESTS
    std::optional<XDRInputFileStream> mTxResultIn;
    std::optional<TransactionHistoryResultEntry> mTxHistoryResultEntry;
#endif // BUILD_TESTS
    LedgerHeaderHistoryEntry mHeaderHistoryEntry;
    OnFailureCallback mOnFailure;

    bool mFilesOpen{false};

    std::shared_ptr<ConditionalWork> mConditionalWork;

    TxSetXDRFrameConstPtr getCurrentTxSet();
#ifdef BUILD_TESTS
    std::optional<TransactionResultSet> getCurrentTxResultSet();
#endif // BUILD_TESTS
    void openInputFiles();

    std::shared_ptr<LedgerCloseData> getNextLedgerCloseData();

    void closeFiles();

  public:
    ApplyCheckpointWork(Application& app, TmpDir const& downloadDir,
                        LedgerRange const& range, OnFailureCallback cb);
    ~ApplyCheckpointWork() = default;
    std::string getStatus() const override;
    void onFailureRaise() override;
    void shutdown() override;

  protected:
    void onReset() override;
    State onRun() override;
    bool onAbort() override;
};
}
