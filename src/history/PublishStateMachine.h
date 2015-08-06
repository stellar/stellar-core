#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Timer.h"
#include "util/TmpDir.h"
#include "history/HistoryArchive.h"

#include <map>
#include <memory>
#include <vector>

namespace soci
{
class transaction;
class session;
}

namespace medida
{
class Counter;
}

namespace stellar
{

class Application;
class Database;
class Bucket;
class BucketList;

enum PublishState
{
    PUBLISH_RETRYING = 0,
    PUBLISH_BEGIN = 1,
    PUBLISH_OBSERVED = 2,
    PUBLISH_SENDING = 3,
    PUBLISH_COMMITTING = 4,
    PUBLISH_END = 5
};

enum FilePublishState
{
    FILE_PUBLISH_FAILED = -1,
    FILE_PUBLISH_NEEDED = 0,
    FILE_PUBLISH_COMPRESSING = 3,
    FILE_PUBLISH_COMPRESSED = 4,
    FILE_PUBLISH_MAKING_DIR = 5,
    FILE_PUBLISH_MADE_DIR = 6,
    FILE_PUBLISH_UPLOADING = 7,
    FILE_PUBLISH_UPLOADED = 8,
};

struct StateSnapshot;
template <typename T> class FileTransferInfo;

class ArchivePublisher : public std::enable_shared_from_this<ArchivePublisher>
{
    Application& mApp;
    std::function<void(asio::error_code const&)> mEndHandler;
    asio::error_code mError;
    PublishState mState;
    size_t mRetryCount;
    VirtualTimer mRetryTimer;

    std::shared_ptr<HistoryArchive> mArchive;
    HistoryArchiveState mArchiveState;
    std::shared_ptr<StateSnapshot> mSnap;

    std::map<std::string, std::shared_ptr<FileTransferInfo<FilePublishState>>>
        mFileInfos;

    void fileStateChange(asio::error_code const& ec,
                         std::string const& hashname,
                         FilePublishState newGoodState);

  public:
    static const size_t kRetryLimit;
    ArchivePublisher(Application& app,
                     std::function<void(asio::error_code const&)> handler,
                     std::shared_ptr<HistoryArchive> archive,
                     std::shared_ptr<StateSnapshot> snap);

    void enterRetryingState();
    void enterBeginState();
    void enterObservedState(HistoryArchiveState const& has);
    void enterSendingState();
    void enterCommittingState();
    void enterEndState();

    bool isDone() const;
};

typedef std::function<void(asio::error_code const&)> PublishCallback;
typedef std::shared_ptr<StateSnapshot> SnapshotPtr;

class PublishStateMachine
{
    Application& mApp;
    std::vector<std::shared_ptr<ArchivePublisher>> mPublishers;
    std::deque<std::pair<SnapshotPtr, PublishCallback>> mPendingSnaps;
    medida::Counter& mPublishersSize;
    medida::Counter& mPendingSnapsSize;
    VirtualTimer mRecheckRunningMergeTimer;

    void writeNextSnapshot();
    void finishOne(asio::error_code const&);

  public:
    PublishStateMachine(Application& app);
    static SnapshotPtr takeSnapshot(Application& app);

    // Returns true if delayed, false if immediately dispatched.
    bool queueSnapshot(SnapshotPtr snap, PublishCallback handler);

    void snapshotWritten(asio::error_code const&);
    void snapshotPublished(asio::error_code const&);
};
}
