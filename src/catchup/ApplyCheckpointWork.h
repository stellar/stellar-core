// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "catchup/ApplyLedgerWork.h"
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

using TmpDirVec = std::vector<std::shared_ptr<TmpDir const>>;

// This struct stores information about SCP messages in a single history
// checkpoint
struct SCPCheckpointInfo
{
    // Input stream holding `SCPHistoryEntry`s
    XDRInputFileStream scpHistoryIn;
    // Most recent SCP history entry read from `scpHistoryIn`
    std::shared_ptr<SCPHistoryEntry> scpHistoryEntry;
};

/**
 * This class is responsible for applying transactions stored in files on
 * temporary directory (downloadDir) to local ledger. It requires two sets of
 * files - ledgers and transactions - int .xdr format. Transaction files are
 * used to read transactions that will be used and ledger files are used to
 * check if ledger hashes are matching.
 *
 * In each run it skips or applies transactions from one ledger. Skipping occurs
 * when ledger to be applied is older than LCL from local ledger. At LCL
 * boundary checks are made to confirm that ledgers from files knit up with
 * LCL. If everything is OK, an apply ledger operation is performed. Then
 * another check is made - if new local ledger matches corresponding ledger from
 * file.
 *
 * Constructor of this class takes some important parameters:
 * * downloadDir - directory containing ledger and transaction files
 * * range - LedgerRange to apply, must be checkpoint-aligned,
 * and cover at most one checkpoint.
 */

class ApplyCheckpointWork : public BasicWork
{
    TmpDir const& mDownloadDir;
    LedgerRange const mLedgerRange;
    uint32_t const mCheckpoint;
    // The directories containing downloaded SCP history. May be null if no such
    // directories exist.
    std::shared_ptr<TmpDirVec const> mSCPDownloadDirs;

    XDRInputFileStream mHdrIn;
    XDRInputFileStream mTxIn;
    TransactionHistoryEntry mTxHistoryEntry;
    LedgerHeaderHistoryEntry mHeaderHistoryEntry;
    // Vector containing each archive's SCP messages for the current checkpoint
    // being processed
    std::vector<SCPCheckpointInfo> mSCPCheckpointInfo;
    OnFailureCallback mOnFailure;

    bool mFilesOpen{false};

    std::shared_ptr<ConditionalWork> mConditionalWork;

    TxSetXDRFrameConstPtr getCurrentTxSet();
    void openInputFiles();

    std::shared_ptr<LedgerCloseData> getNextLedgerCloseData();

    // Returns a vector holding SCP messages from each archive for the ledger
    // being processed.  This vector may be smaller than the total number of
    // archives if some archives did not contain messages for the ledger.
    std::unique_ptr<SCPHistoryEntryVec> getNextSCPHistoryEntries();

    void closeFiles();

  public:
    ApplyCheckpointWork(Application& app, TmpDir const& downloadDir,
                        LedgerRange const& range, OnFailureCallback cb,
                        std::shared_ptr<TmpDirVec const>& scpDownloadDirs);
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
