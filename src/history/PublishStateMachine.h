#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC


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

namespace stellar
{

class Application;
class Database;
class Bucket;
class BucketList;

enum PublishState
{
    PUBLISH_RETRYING,
    PUBLISH_BEGIN,
    PUBLISH_OBSERVED,
    PUBLISH_SENDING,
    PUBLISH_COMMITTING,
    PUBLISH_END
};

enum FilePublishState
{
    FILE_PUBLISH_FAILED = -1,
    FILE_PUBLISH_NEEDED = 0,
    FILE_PUBLISH_COMPRESSING = 3,
    FILE_PUBLISH_COMPRESSED = 4,
    FILE_PUBLISH_UPLOADING = 5,
    FILE_PUBLISH_UPLOADED = 6,
};

struct StateSnapshot;
template <typename T> class FileTransferInfo;

class
ArchivePublisher : public std::enable_shared_from_this<ArchivePublisher>
{
    static const size_t kRetryLimit;

    Application& mApp;
    std::function<void(asio::error_code const&)> mEndHandler;
    asio::error_code mError;
    PublishState mState;
    size_t mRetryCount;
    VirtualTimer mRetryTimer;

    std::shared_ptr<HistoryArchive> mArchive;
    HistoryArchiveState mArchiveState;
    std::shared_ptr<StateSnapshot> mSnap;

    std::map<std::string, std::shared_ptr<FileTransferInfo<FilePublishState>>> mFileInfos;

    void fileStateChange(asio::error_code const& ec,
                         std::string const& hashname,
                         FilePublishState newGoodState);

public:
    ArchivePublisher(Application& app,
                     std::function<void(asio::error_code const&)> handler,
                     std::shared_ptr<HistoryArchive> archive,
                     std::shared_ptr<StateSnapshot> snap);

    std::shared_ptr<HistoryArchive> getArchive();

    void enterRetryingState();
    void enterBeginState();
    void enterObservedState(HistoryArchiveState const& has);
    void enterSendingState();
    void enterCommittingState();
    void enterEndState();

    bool isDone() const;
};

class
PublishStateMachine
{
    Application& mApp;
    std::function<void(asio::error_code const&)> mEndHandler;
    asio::error_code mError;
    std::vector<std::shared_ptr<ArchivePublisher>> mPublishers;
    void takeSnapshot();
public:
    PublishStateMachine(Application& app,
                        std::function<void(asio::error_code const&)> handler);

    void snapshotTaken(asio::error_code const&,
                       std::shared_ptr<StateSnapshot>);
    void snapshotPublished(asio::error_code const&);
};


}
