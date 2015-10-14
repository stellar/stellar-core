#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "history/HistoryArchive.h"
#include "history/HistoryManager.h"
#include "bucket/Bucket.h"
#include "util/Timer.h"
#include "util/TmpDir.h"

#include <map>
#include <memory>

namespace stellar
{

/**
 * This is a callback-driven class that tries to "catch up" a given
 * node's history to the current consensus ledger of the network.
 *
 * It does so by downloading files of transaction history and buckets
 * of ledger objects, and applying them to the current database.
 *
 * If any of these downloads or applies fail, it retries a certain
 * number of times before giving up entirely.
 *
 * The state transitions are as follows:
 *
 *
 *  --> BEGIN (initial state) <--- (retries > R)-\
 *        |                                      |
 *    (ask history archive for state,            |
 *     define this as "anchor ledger")           ^
 *        |                                      |
 *        V                                      |
 *      ANCHORED <--(anchored && -----<--- RETRYING (time-delay state)
 *        |          retries < R)                |      |
 *        |                                      |      |
 *        |                                      ^      V
 *    (download, decompress, verify              |      |
 *     missing buckets and history)              |      |
 *        |                                      |      (verifying &&
 *        V                                      |      |retries < R)
 *     FETCHING ---- (>0 fetches failed) -->-----/      |
 *        |                                      |      V
 *    (all fetches ok)                           ^      |
 *        |                                      |      |
 *        /--------------------------------<---- | --<--/
 *        |
 *        V                                      |
 *     VERIFYING ------(VERIFY_HASH_UNKNOWN)-----/
 *        |                                      |
 *    (VERIFY_HASH_OK)                           |
 *        |                                      ^
 *        V                                      |
 *     APPLYING ----- (DB errors) ---------------/
 *        |
 *   (apply succeeded)
 *        |
 *        V
 *       END --> (terminal state, call callback)
 *
 */
enum CatchupState
{
    CATCHUP_BEGIN,
    CATCHUP_RETRYING,
    CATCHUP_ANCHORED,
    CATCHUP_FETCHING,
    CATCHUP_VERIFYING,
    CATCHUP_APPLYING,
    CATCHUP_END
};

enum FileCatchupState
{
    FILE_CATCHUP_FAILED = 0,
    FILE_CATCHUP_NEEDED = 1,
    FILE_CATCHUP_DOWNLOADING = 2,
    FILE_CATCHUP_DOWNLOADED = 3,
    FILE_CATCHUP_DECOMPRESSING = 4,
    FILE_CATCHUP_DECOMPRESSED = 5,
    FILE_CATCHUP_VERIFYING = 6,
    FILE_CATCHUP_VERIFIED = 7
};

template <typename T> class FileTransferInfo;
typedef FileTransferInfo<FileCatchupState> FileCatchupInfo;

class HistoryArchive;
struct HistoryArchiveState;
class Application;

class CatchupStateMachine
    : public std::enable_shared_from_this<CatchupStateMachine>
{

  private:
    static const size_t kRetryLimit;

    Application& mApp;
    uint32_t mInitLedger;
    uint32_t mNextLedger;
    LedgerHeaderHistoryEntry mLastClosed;
    HistoryManager::CatchupMode mMode;
    std::function<void(asio::error_code const& ec,
                       HistoryManager::CatchupMode mode,
                       LedgerHeaderHistoryEntry const& lastClosed)> mEndHandler;
    asio::error_code mError;
    CatchupState mState;
    size_t mRetryCount;
    VirtualTimer mRetryTimer;
    TmpDir mDownloadDir;

    std::shared_ptr<HistoryArchive> mArchive;
    HistoryArchiveState mLocalState;
    HistoryArchiveState mArchiveState;
    std::map<std::string, std::shared_ptr<FileTransferInfo<FileCatchupState>>>
        mFileInfos;
    std::map<uint32_t, std::shared_ptr<FileTransferInfo<FileCatchupState>>>
        mHeaderInfos;
    std::map<uint32_t, std::shared_ptr<FileTransferInfo<FileCatchupState>>>
        mTransactionInfos;
    std::map<std::string, std::shared_ptr<Bucket>> mBuckets;

    std::shared_ptr<Bucket> getBucketToApply(std::string const& hash);

    std::shared_ptr<HistoryArchive> selectRandomReadableHistoryArchive();
    void fileStateChange(asio::error_code const& ec,
                         std::string const& hashname,
                         FileCatchupState newGoodState);

    std::shared_ptr<FileCatchupInfo> queueTransactionsFile(uint32_t snap);
    std::shared_ptr<FileCatchupInfo> queueLedgerFile(uint32_t snap);

    void enterBeginState();
    void enterAnchoredState(HistoryArchiveState const& has);
    void enterRetryingState(uint64_t nseconds = 2);
    bool
    advanceFileState(std::shared_ptr<FileTransferInfo<FileCatchupState>> fi);
    void enterFetchingState(
        std::shared_ptr<FileTransferInfo<FileCatchupState>> fi = nullptr);

    void enterVerifyingState();
    void advanceVerifyingState(std::shared_ptr<LedgerHeaderHistoryEntry> prev,
                               uint32_t checkpoint);
    HistoryManager::VerifyHashStatus verifyHistoryOfSingleCheckpoint(
        std::shared_ptr<LedgerHeaderHistoryEntry> prev, uint32_t checkpoint);
    void finishVerifyingState(HistoryManager::VerifyHashStatus status);

    struct ApplyState;
    std::shared_ptr<ApplyState> mApplyState;

    void enterApplyingState();
    void advanceApplyingState();

    void enterEndState();

    void applySingleBucketLevel(bool& applying, size_t& level);
    void acquireFinalLedgerState(uint32_t ledgerNum);
    void applyHistoryOfSingleCheckpoint(uint32_t checkpoint);

  public:
    CatchupStateMachine(
        Application& app, uint32_t initLedger, HistoryManager::CatchupMode mode,
        HistoryArchiveState localState,
        std::function<
            void(asio::error_code const& ec, HistoryManager::CatchupMode mode,
                 LedgerHeaderHistoryEntry const& lastClosed)> handler);

    void begin();

    static const std::chrono::seconds SLEEP_SECONDS_PER_LEDGER;
};
}
