#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "history/HistoryArchive.h"
#include "clf/Bucket.h"
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
 *     define this as "anchor ledger")           |
 *        |                                      |
 *        V                                      |
 *      ANCHORED <----- (retries < R) -------- RETRYING (time-delay state)
 *        |                                      ^
 *    (download, decompress, verify              |
 *     missing buckets and history)              |
 *        |                                      |
 *        V                                      |
 *     FETCHING ---- (>0 fetches failed) --------/
 *        |                                      |
 *    (all fetches ok)                           |
 *        |                                      |
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

class HistoryArchive;
struct HistoryArchiveState;
class Application;

class
CatchupStateMachine
{
    static const size_t kRetryLimit;

    Application& mApp;
    std::function<void(asio::error_code const&)> mEndHandler;
    asio::error_code mError;
    CatchupState mState;
    size_t mRetryCount;
    VirtualTimer mRetryTimer;
    TmpDir mDownloadDir;

    std::shared_ptr<HistoryArchive> mArchive;
    HistoryArchiveState mLocalState;
    HistoryArchiveState mArchiveState;
    std::map<std::string, std::shared_ptr<FileTransferInfo<FileCatchupState>>> mFileInfos;
    std::map<uint32_t, std::shared_ptr<FileTransferInfo<FileCatchupState>>> mHeaderInfos;
    std::map<uint32_t, std::shared_ptr<FileTransferInfo<FileCatchupState>>> mTransactionInfos;
    std::map<std::string, std::shared_ptr<Bucket>> mBuckets;

    std::shared_ptr<HistoryArchive> selectRandomReadableHistoryArchive();
    void fileStateChange(asio::error_code const& ec,
                         std::string const& hashname,
                         FileCatchupState newGoodState);

    void enterBeginState();
    void enterAnchoredState(HistoryArchiveState const& has);
    void enterRetryingState();
    void enterFetchingState();
    void enterApplyingState();
    void enterEndState();

public:

    CatchupStateMachine(Application& app,
                        std::function<void(asio::error_code const&)> handler);


};

}
