#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Timer.h"
#include "util/TmpDir.h"
#include "history/HistoryArchive.h"
#include "history/StateSnapshot.h"

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

class PublishStateMachine;

class ArchivePublisher : public std::enable_shared_from_this<ArchivePublisher>
{
    Application& mApp;
    PublishStateMachine& mPublish;
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
                     PublishStateMachine& mPublish,
                     std::shared_ptr<HistoryArchive> archive,
                     std::shared_ptr<StateSnapshot> snap);

    void enterRetryingState();
    void enterBeginState();
    void enterObservedState(HistoryArchiveState const& has);
    void enterSendingState();
    void enterCommittingState();
    void enterEndState();

    bool isDone() const;
    bool failed() const;
};

typedef std::shared_ptr<StateSnapshot> SnapshotPtr;

class PublishStateMachine
{
    Application& mApp;
    std::vector<std::shared_ptr<ArchivePublisher>> mPublishers;
    SnapshotPtr mPendingSnap;
    medida::Counter& mPublishersSize;
    VirtualTimer mRecheckRunningMergeTimer;

    void writePendingSnapshot();
    void finishOne(bool success);

  public:
    PublishStateMachine(Application& app);
    static SnapshotPtr takeSnapshot(Application& app,
                                    HistoryArchiveState const& state);

    bool currentlyPublishing() const { return !!mPendingSnap; };
    void publishSnapshot(SnapshotPtr snap);

    void snapshotWritten(asio::error_code const&);
    void snapshotPublished();
};
}
