// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/Bucket.h"
#include "bucket/BucketApplicator.h"
#include "bucket/BucketList.h"
#include "herder/TxSetFrame.h"
#include "history/FileTransferInfo.h"
#include "history/HistoryArchive.h"
#include "history/HistoryManager.h"
#include "main/Application.h"
#include "util/TmpDir.h"
#include "work/WorkManager.h"

#include <map>
#include <memory>
#include <string>

/*
 * This file contains a variety of Work subclasses for the History subsystem.
 */

namespace stellar
{

// This subclass exists for two reasons: first, to factor out a little code
// around running commands, and second to ensure that command-running
// happens from onStart rather than onRun, and that onRun is an empty
// method; this way we only run a command _once_ (when it's first
// scheduled) rather than repeatedly (racing with other copies of itself)
// when rescheduled.
class RunCommandWork : public Work
{
    virtual void getCommand(std::string& cmdLine, std::string& outFile) = 0;

  public:
    RunCommandWork(Application& app, WorkParent& parent,
                   std::string const& uniqueName,
                   size_t maxRetries = Work::RETRY_A_FEW);
    void onStart() override;
    void onRun() override;
};

class GetRemoteFileWork : public RunCommandWork
{
    std::string mRemote;
    std::string mLocal;
    std::shared_ptr<HistoryArchive const> mArchive;
    void getCommand(std::string& cmdLine, std::string& outFile) override;

  public:
    // Passing `nullptr` for the archive argument will cause the work to
    // select a new readable history archive at random each time it runs /
    // retries.
    GetRemoteFileWork(Application& app, WorkParent& parent,
                      std::string const& remote, std::string const& local,
                      std::shared_ptr<HistoryArchive const> archive = nullptr,
                      size_t maxRetries = Work::RETRY_A_FEW);
    void onReset() override;
};

class PutRemoteFileWork : public RunCommandWork
{
    std::string mRemote;
    std::string mLocal;
    std::shared_ptr<HistoryArchive const> mArchive;
    void getCommand(std::string& cmdLine, std::string& outFile) override;

  public:
    PutRemoteFileWork(Application& app, WorkParent& parent,
                      std::string const& remote, std::string const& local,
                      std::shared_ptr<HistoryArchive const> archive);
};

class MakeRemoteDirWork : public RunCommandWork
{
    std::string mDir;
    std::shared_ptr<HistoryArchive const> mArchive;
    void getCommand(std::string& cmdLine, std::string& outFile) override;

  public:
    MakeRemoteDirWork(Application& app, WorkParent& parent,
                      std::string const& dir,
                      std::shared_ptr<HistoryArchive const> archive);
};

class GzipFileWork : public RunCommandWork
{
    std::string mFilenameNoGz;
    bool mKeepExisting;
    void getCommand(std::string& cmdLine, std::string& outFile) override;

  public:
    GzipFileWork(Application& app, WorkParent& parent,
                 std::string const& filenameNoGz, bool keepExisting = false);
    void onReset() override;
};

class GunzipFileWork : public RunCommandWork
{
    std::string mFilenameGz;
    bool mKeepExisting;
    void getCommand(std::string& cmdLine, std::string& outFile) override;

  public:
    GunzipFileWork(Application& app, WorkParent& parent,
                   std::string const& filenameGz, bool keepExisting = false,
                   size_t maxRetries = Work::RETRY_A_FEW);
    void onReset() override;
};

class VerifyBucketWork : public Work
{
    std::map<std::string, std::shared_ptr<Bucket>>& mBuckets;
    std::string mBucketFile;
    uint256 mHash;

  public:
    VerifyBucketWork(Application& app, WorkParent& parent,
                     std::map<std::string, std::shared_ptr<Bucket>>& buckets,
                     std::string const& bucketFile, uint256 const& hash);
    void onRun() override;
    void onStart() override;
    Work::State onSuccess() override;
};

class ApplyBucketsWork : public Work
{
    std::map<std::string, std::shared_ptr<Bucket>>& mBuckets;
    HistoryArchiveState& mApplyState;
    LedgerHeaderHistoryEntry const& mFirstVerified;

    bool mApplying;
    size_t mLevel;
    std::shared_ptr<Bucket> mSnapBucket;
    std::shared_ptr<Bucket> mCurrBucket;
    std::unique_ptr<BucketApplicator> mSnapApplicator;
    std::unique_ptr<BucketApplicator> mCurrApplicator;

    std::shared_ptr<Bucket> getBucket(std::string const& bucketHash);
    BucketLevel& getBucketLevel(size_t level);
    BucketList& getBucketList();

  public:
    ApplyBucketsWork(Application& app, WorkParent& parent,
                     std::map<std::string, std::shared_ptr<Bucket>>& buckets,
                     HistoryArchiveState& applyState,
                     LedgerHeaderHistoryEntry const& firstVerified);

    void onReset() override;
    void onStart() override;
    void onRun() override;
    Work::State onSuccess() override;
};

class GetHistoryArchiveStateWork : public Work
{
    HistoryArchiveState& mState;
    uint32_t mSeq;
    VirtualClock::duration mInitialDelay;
    std::shared_ptr<HistoryArchive const> mArchive;
    std::string mLocalFilename;

  public:
    GetHistoryArchiveStateWork(
        Application& app, WorkParent& parent, HistoryArchiveState& state,
        uint32_t seq = 0,
        VirtualClock::duration const& intitialDelay = std::chrono::seconds(0),
        std::shared_ptr<HistoryArchive const> archive = nullptr,
        size_t maxRetries = Work::RETRY_A_FEW);
    std::string getStatus() const override;
    VirtualClock::duration getRetryDelay() const override;
    void onReset() override;
    void onRun() override;
};

class PutHistoryArchiveStateWork : public Work
{
    HistoryArchiveState const& mState;
    std::shared_ptr<HistoryArchive const> mArchive;
    std::string mLocalFilename;
    std::shared_ptr<Work> mPutRemoteFileWork;

  public:
    PutHistoryArchiveStateWork(Application& app, WorkParent& parent,
                               HistoryArchiveState const& state,
                               std::shared_ptr<HistoryArchive const> archive);
    void onReset() override;
    void onRun() override;
    Work::State onSuccess() override;
};

class GetAndUnzipRemoteFileWork : public Work
{
    std::shared_ptr<Work> mGetRemoteFileWork;
    std::shared_ptr<Work> mGunzipFileWork;

    FileTransferInfo mFt;
    std::shared_ptr<HistoryArchive const> mArchive;

  public:
    // Passing `nullptr` for the archive argument will cause the work to
    // select a new readable history archive at random each time it runs /
    // retries.
    GetAndUnzipRemoteFileWork(
        Application& app, WorkParent& parent, FileTransferInfo ft,
        std::shared_ptr<HistoryArchive const> archive = nullptr,
        size_t maxRetries = Work::RETRY_A_FEW);
    std::string getStatus() const override;
    void onReset() override;
    Work::State onSuccess() override;
    void onFailureRaise() override;
};

class BucketDownloadWork : public Work
{
  protected:
    HistoryArchiveState mLocalState;
    std::unique_ptr<TmpDir> mDownloadDir;
    std::map<std::string, std::shared_ptr<Bucket>> mBuckets;

  public:
    BucketDownloadWork(Application& app, WorkParent& parent,
                       std::string const& uniqueName,
                       HistoryArchiveState const& localState);
    void onReset() override;
    void takeDownloadDir(BucketDownloadWork& other);
};

class CatchupWork : public BucketDownloadWork
{
  protected:
    HistoryArchiveState mRemoteState;
    LedgerHeaderHistoryEntry mFirstVerified;
    LedgerHeaderHistoryEntry mLastVerified;
    LedgerHeaderHistoryEntry mLastApplied;
    uint32_t const mInitLedger;
    bool const mManualCatchup;
    std::shared_ptr<Work> mGetHistoryArchiveStateWork;

    uint32_t nextLedger() const;
    virtual uint32_t firstCheckpointSeq() const = 0;
    uint32_t lastCheckpointSeq() const;

  public:
    CatchupWork(Application& app, WorkParent& parent, uint32_t initLedger,
                std::string const& mode, bool manualCatchup);
    virtual void onReset() override;
};

class CatchupMinimalWork : public CatchupWork
{
  public:
    typedef std::function<void(asio::error_code const& ec,
                               HistoryManager::CatchupMode mode,
                               LedgerHeaderHistoryEntry const& lastClosed)>
        handler;

  protected:
    std::shared_ptr<Work> mDownloadLedgersWork;
    std::shared_ptr<Work> mVerifyLedgersWork;
    std::shared_ptr<Work> mDownloadBucketsWork;
    std::shared_ptr<Work> mApplyWork;
    handler mEndHandler;
    virtual uint32_t firstCheckpointSeq() const override;

  public:
    CatchupMinimalWork(Application& app, WorkParent& parent,
                       uint32_t initLedger, bool manualCatchup,
                       handler endHandler);
    std::string getStatus() const override;
    void onReset() override;
    Work::State onSuccess() override;
    void onFailureRaise() override;
};

class BatchDownloadWork : public Work
{
    // Specialized class for downloading _lots_ of files (thousands to
    // millions). Sets up N (small number) of parallel download-decompress
    // worker chains to nibble away at a set of files-to-download, stored
    // as an integer deque. N is the subprocess-concurrency limit by default
    // (though it's still enforced globally at the ProcessManager level,
    // so you don't have to worry about making a few extra BatchDownloadWork
    // classes -- they won't override the global limit, just schedule a small
    // backlog in the ProcessManager).
    std::deque<uint32_t> mFinished;
    std::map<std::string, uint32_t> mRunning;
    uint32_t mFirst;
    uint32_t mLast;
    uint32_t mNext;
    std::string mFileType;
    TmpDir const& mDownloadDir;

    void addNextDownloadWorker();

  public:
    BatchDownloadWork(Application& app, WorkParent& parent, uint32_t first,
                      uint32_t last, std::string const& type,
                      TmpDir const& downloadDir);
    std::string getStatus() const override;
    void onReset() override;
    void notify(std::string const& childChanged) override;
};

class CatchupCompleteWork : public CatchupWork
{

    typedef std::function<void(asio::error_code const& ec,
                               HistoryManager::CatchupMode mode,
                               LedgerHeaderHistoryEntry const& lastClosed)>
        handler;

    std::shared_ptr<Work> mDownloadLedgersWork;
    std::shared_ptr<Work> mDownloadTransactionsWork;
    std::shared_ptr<Work> mVerifyWork;
    std::shared_ptr<Work> mApplyWork;
    handler mEndHandler;
    virtual uint32_t firstCheckpointSeq() const override;

  public:
    CatchupCompleteWork(Application& app, WorkParent& parent,
                        uint32_t initLedger, bool manualCatchup,
                        handler endHandler);
    std::string getStatus() const override;
    void onReset() override;
    Work::State onSuccess() override;
    void onFailureRaise() override;
};

// Catchup-recent is just a catchup-minimal to (now - N),
// followed by a catchup-complete to now.
class CatchupRecentWork : public Work
{
  public:
    typedef std::function<void(asio::error_code const& ec,
                               HistoryManager::CatchupMode mode,
                               LedgerHeaderHistoryEntry const& ledger)>
        handler;

  protected:
    std::shared_ptr<Work> mCatchupMinimalWork;
    std::shared_ptr<Work> mCatchupCompleteWork;
    uint32_t mInitLedger;
    bool mManualCatchup;
    handler mEndHandler;
    LedgerHeaderHistoryEntry mFirstVerified;
    LedgerHeaderHistoryEntry mLastApplied;

    handler writeFirstVerified();
    handler writeLastApplied();

  public:
    CatchupRecentWork(Application& app, WorkParent& parent, uint32_t initLedger,
                      bool manualCatchup, handler endHandler);
    std::string getStatus() const override;
    void onReset() override;
    Work::State onSuccess() override;
    void onFailureRaise() override;
};

class VerifyLedgerChainWork : public Work
{
    TmpDir const& mDownloadDir;
    uint32_t mFirstSeq;
    uint32_t mCurrSeq;
    uint32_t mLastSeq;
    bool mManualCatchup;
    LedgerHeaderHistoryEntry& mFirstVerified;
    LedgerHeaderHistoryEntry& mLastVerified;

    HistoryManager::VerifyHashStatus verifyHistoryOfSingleCheckpoint();

  public:
    VerifyLedgerChainWork(Application& app, WorkParent& parent,
                          TmpDir const& downloadDir, uint32_t firstSeq,
                          uint32_t lastSeq, bool manualCatchup,
                          LedgerHeaderHistoryEntry& firstVerified,
                          LedgerHeaderHistoryEntry& lastVerified);
    std::string getStatus() const override;
    void onReset() override;
    Work::State onSuccess() override;
};

class ApplyLedgerChainWork : public Work
{
    TmpDir const& mDownloadDir;
    uint32_t mFirstSeq;
    uint32_t mCurrSeq;
    uint32_t mLastSeq;
    XDRInputFileStream mHdrIn;
    XDRInputFileStream mTxIn;
    TransactionHistoryEntry mTxHistoryEntry;
    LedgerHeaderHistoryEntry& mLastApplied;

    TxSetFramePtr getCurrentTxSet();
    void openCurrentInputFiles();
    bool applyHistoryOfSingleLedger();

  public:
    ApplyLedgerChainWork(Application& app, WorkParent& parent,
                         TmpDir const& downloadDir, uint32_t first,
                         uint32_t last, LedgerHeaderHistoryEntry& lastApplied);
    std::string getStatus() const override;
    void onReset() override;
    void onStart() override;
    void onRun() override;
    Work::State onSuccess() override;
};

class ResolveSnapshotWork : public Work
{
    std::shared_ptr<StateSnapshot> mSnapshot;

  public:
    ResolveSnapshotWork(Application& app, WorkParent& parent,
                        std::shared_ptr<StateSnapshot> snapshot);
    void onRun() override;
};

class WriteSnapshotWork : public Work
{
    std::shared_ptr<StateSnapshot> mSnapshot;

  public:
    WriteSnapshotWork(Application& app, WorkParent& parent,
                      std::shared_ptr<StateSnapshot> snapshot);
    void onStart() override;
    void onRun() override;
};

class PutSnapshotFilesWork : public Work
{
    std::shared_ptr<HistoryArchive const> mArchive;
    std::shared_ptr<StateSnapshot> mSnapshot;
    HistoryArchiveState mRemoteState;

    std::shared_ptr<Work> mGetHistoryArchiveStateWork;
    std::shared_ptr<Work> mPutFilesWork;
    std::shared_ptr<Work> mPutHistoryArchiveStateWork;

  public:
    PutSnapshotFilesWork(Application& app, WorkParent& parent,
                         std::shared_ptr<HistoryArchive const> archive,
                         std::shared_ptr<StateSnapshot> snapshot);
    void onReset() override;
    Work::State onSuccess() override;
};

class PublishWork : public Work
{
    std::shared_ptr<StateSnapshot> mSnapshot;

    std::shared_ptr<Work> mResolveSnapshotWork;
    std::shared_ptr<Work> mWriteSnapshotWork;
    std::shared_ptr<Work> mUpdateArchivesWork;

  public:
    PublishWork(Application& app, WorkParent& parent,
                std::shared_ptr<StateSnapshot> snapshot);
    std::string getStatus() const override;
    void onReset() override;
    void onFailureRaise() override;
    Work::State onSuccess() override;
};

class RepairMissingBucketsWork : public BucketDownloadWork
{

    typedef std::function<void(asio::error_code const& ec)> handler;
    handler mEndHandler;

  public:
    RepairMissingBucketsWork(Application& app, WorkParent& parent,
                             HistoryArchiveState const& localState,
                             handler endHandler);
    void onReset() override;
    void onFailureRaise() override;
    Work::State onSuccess() override;
};

class FetchRecentQsetsWork : public Work
{

    typedef std::function<void(asio::error_code const& ec)> handler;
    handler mEndHandler;
    std::unique_ptr<TmpDir> mDownloadDir;
    InferredQuorum& mInferredQuorum;
    HistoryArchiveState mRemoteState;
    std::shared_ptr<Work> mGetHistoryArchiveStateWork;
    std::shared_ptr<Work> mDownloadSCPMessagesWork;

  public:
    FetchRecentQsetsWork(Application& app, WorkParent& parent,
                         InferredQuorum& iq, handler endHandler);
    void onReset() override;
    void onFailureRaise() override;
    Work::State onSuccess() override;
};
}
