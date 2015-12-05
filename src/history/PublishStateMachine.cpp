// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "history/PublishStateMachine.h"
#include "bucket/Bucket.h"
#include "bucket/BucketList.h"
#include "bucket/BucketManager.h"
#include "crypto/Hex.h"
#include "history/HistoryArchive.h"
#include "history/HistoryManager.h"
#include "history/FileTransferInfo.h"
#include "history/StateSnapshot.h"
#include "main/Application.h"
#include "main/Config.h"
#include "database/Database.h"
#include "ledger/LedgerHeaderFrame.h"
#include "transactions/TransactionFrame.h"
#include "util/Logging.h"
#include "util/make_unique.h"
#include "util/XDRStream.h"
#include "herder/Herder.h"

#include "medida/metrics_registry.h"
#include "medida/counter.h"

#include <soci.h>

namespace stellar
{

const size_t ArchivePublisher::kRetryLimit = 16;

ArchivePublisher::ArchivePublisher(
    Application& app,
    PublishStateMachine& pub,
    std::shared_ptr<HistoryArchive> archive,
    std::shared_ptr<StateSnapshot> snap)
    : mApp(app)
    , mPublish(pub)
    , mState(PUBLISH_RETRYING)
    , mRetryCount(0)
    , mRetryTimer(app)
    , mArchive(archive)
    , mSnap(snap)
{
}

void
ArchivePublisher::enterRetryingState()
{
    assert(mState == PUBLISH_BEGIN || mState == PUBLISH_SENDING ||
           mState == PUBLISH_COMMITTING || mState == PUBLISH_RETRYING);

    if (mState == PUBLISH_RETRYING)
    {
        return;
    }

    mState = PUBLISH_RETRYING;
    mRetryTimer.expires_from_now(std::chrono::seconds(2));
    std::weak_ptr<ArchivePublisher> weak(shared_from_this());
    mRetryTimer.async_wait(
        [weak](asio::error_code const& ec)
        {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }
            if (self->mRetryCount++ > kRetryLimit)
            {
                CLOG(WARNING, "History")
                    << "Retry count " << kRetryLimit
                    << " exceeded, abandonning  publish-attempt on archive '"
                    << self->mArchive->getName() << "'";
                self->enterEndState();
            }
            else
            {
                CLOG(INFO, "History") << "Retrying publish to archive '"
                                      << self->mArchive->getName() << "'";
                self->enterBeginState();
            }
        });
}

void
ArchivePublisher::enterBeginState()
{
    assert(mState == PUBLISH_RETRYING);
    mState = PUBLISH_BEGIN;
    std::weak_ptr<ArchivePublisher> weak(shared_from_this());
    mArchive->getMostRecentState(
        mApp, [weak](asio::error_code const& ec, HistoryArchiveState const& has)
        {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }
            if (ec)
            {
                CLOG(WARNING, "History") << "Publisher failed to retrieve "
                                            "state from history archive '"
                                         << self->mArchive->getName()
                                         << "', restarting publish";
                self->enterRetryingState();
            }
            else
            {
                self->enterObservedState(has);
            }
        });
}

void
ArchivePublisher::enterObservedState(HistoryArchiveState const& has)
{
    assert(mState == PUBLISH_BEGIN);
    mState = PUBLISH_OBSERVED;

    mArchiveState = has;
    std::vector<std::string> bucketsToSend =
        mSnap->mLocalState.differingBuckets(mArchiveState);
    std::map<std::string, std::shared_ptr<Bucket>> bucketsByHash;
    for (auto b : mSnap->mLocalBuckets)
    {
        bucketsByHash[binToHex(b->getHash())] = b;
    }

    std::vector<std::shared_ptr<FilePublishInfo>> filePublishInfos = {
        mSnap->mLedgerSnapFile, mSnap->mTransactionSnapFile,
        mSnap->mTransactionResultSnapFile, mSnap->mSCPHistorySnapFile};

    for (auto const& hash : bucketsToSend)
    {
        auto b = bucketsByHash[hash];
        assert(b);
        filePublishInfos.push_back(
            std::make_shared<FilePublishInfo>(FILE_PUBLISH_NEEDED, *b));
    }

    for (auto pi : filePublishInfos)
    {
        auto name = pi->baseName_nogz();
        auto i = mFileInfos.find(name);
        if (i == mFileInfos.end() ||
            i->second->getState() == FILE_PUBLISH_FAILED)
        {
            CLOG(DEBUG, "History") << "Queueing file " << name
                                   << " to send to archive '"
                                   << mArchive->getName() << "'";
            mFileInfos[name] = pi;
        }
        else
        {
            CLOG(DEBUG, "History")
                << "Not queueing file " << name << " to send to archive '"
                << mArchive->getName() << "'; file already queued";
        }
    }
    enterSendingState();
}

/**
 * If `ec` is an error, set the state for `name` to FILE_FAILED, otherwise
 * set it to `newGoodState`. In either case, re-enter FETCHING state.
 */
void
ArchivePublisher::fileStateChange(asio::error_code const& ec,
                                  std::string const& name,
                                  FilePublishState newGoodState)
{
    FilePublishState newState = newGoodState;
    if (ec)
    {
        CLOG(WARNING, "History") << "Publish action failed on " << name;
        newState = FILE_PUBLISH_FAILED;
        mError = ec;
    }
    mFileInfos[name]->setState(newState);

    // Only re-enter "sending" if we aren't retrying or ending.
    if (mState == PUBLISH_OBSERVED || mState == PUBLISH_SENDING)
    {
        enterSendingState();
    }
}

void
ArchivePublisher::enterSendingState()
{
    assert(mState == PUBLISH_OBSERVED || mState == PUBLISH_SENDING);
    mState = PUBLISH_SENDING;

    FilePublishState minimumState = FILE_PUBLISH_COMPLETE;
    auto& hm = mApp.getHistoryManager();
    std::weak_ptr<ArchivePublisher> weak(shared_from_this());

    for (auto& pair : mFileInfos)
    {
        auto fi = pair.second;
        std::string name = fi->baseName_nogz();

        switch (fi->getState())
        {
        case FILE_PUBLISH_FAILED:
            break;

        case FILE_PUBLISH_MAYBE_NEEDED:
            if (!fs::exists(fi->localPath_nogz()))
            {
                fi->setState(FILE_PUBLISH_COMPLETE);
                break;
            }
        // otherwise, fall through

        case FILE_PUBLISH_NEEDED:
            fi->setState(FILE_PUBLISH_COMPRESSING);
            CLOG(DEBUG, "History") << "Compressing " << name;
            hm.compress(fi->localPath_nogz(),
                        [weak, name](asio::error_code const& ec)
                        {
                            auto self = weak.lock();
                            if (!self)
                            {
                                return;
                            }
                            self->fileStateChange(ec, name,
                                                  FILE_PUBLISH_COMPRESSED);
                        },
                        true);
            break;

        case FILE_PUBLISH_COMPRESSING:
            break;

        case FILE_PUBLISH_COMPRESSED:
            fi->setState(FILE_PUBLISH_MAKING_DIR);
            CLOG(DEBUG, "History") << "Making remote directory "
                                   << fi->remoteDir();
            hm.mkdir(mArchive, fi->remoteDir(),
                     [weak, name](asio::error_code const& ec)
                     {
                         auto self = weak.lock();
                         if (!self)
                         {
                             return;
                         }
                         self->fileStateChange(ec, name, FILE_PUBLISH_MADE_DIR);
                     });
            break;

        case FILE_PUBLISH_MAKING_DIR:
            break;

        case FILE_PUBLISH_MADE_DIR:
            fi->setState(FILE_PUBLISH_UPLOADING);
            CLOG(INFO, "History") << "Publishing " << name;
            hm.putFile(mArchive, fi->localPath_gz(), fi->remoteName(),
                       [weak, name](asio::error_code const& ec)
                       {
                           auto self = weak.lock();
                           if (!self)
                           {
                               return;
                           }
                           self->fileStateChange(ec, name,
                                                 FILE_PUBLISH_UPLOADED);
                       });
            break;

        case FILE_PUBLISH_UPLOADING:
            break;

        case FILE_PUBLISH_UPLOADED:
            std::remove(fi->localPath_gz().c_str());
            fi->setState(FILE_PUBLISH_COMPLETE);
        // fall through

        case FILE_PUBLISH_COMPLETE:
            break;
        }

        minimumState = std::min(fi->getState(), minimumState);
    }

    if (minimumState == FILE_PUBLISH_FAILED)
    {
        CLOG(WARNING, "History") << "Some file-puts failed, retrying";
        enterRetryingState();
    }
    else if (minimumState == FILE_PUBLISH_COMPLETE)
    {
        CLOG(INFO, "History") << "All file-puts succeeded";
        enterCommittingState();
    }
    else
    {
        CLOG(DEBUG, "History") << "Some file-puts still in progress";
        // Do nothing here; in-progress states have callbacks set up already
        // which will fire when they complete.
    }
}

void
ArchivePublisher::enterCommittingState()
{
    assert(mState == PUBLISH_SENDING);
    mState = PUBLISH_COMMITTING;
    std::weak_ptr<ArchivePublisher> weak(shared_from_this());
    mArchive->putState(
        mApp, mSnap->mLocalState, [weak](asio::error_code const& ec)
        {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }
            if (ec)
            {
                CLOG(WARNING, "History")
                    << "Publisher failed to update state in history archive '"
                    << self->mArchive->getName() << "', restarting publish";
                self->enterRetryingState();
            }
            else
            {
                self->enterEndState();
            }
        });
}

void
ArchivePublisher::enterEndState()
{
    CLOG(DEBUG, "History") << "Finished publishing to archive '"
                           << this->mArchive->getName() << "'";
    mState = PUBLISH_END;
    mPublish.snapshotPublished();
}

bool
ArchivePublisher::isDone() const
{
    return mState == PUBLISH_END;
}

bool
ArchivePublisher::failed() const
{
    return !!mError;
}

PublishStateMachine::PublishStateMachine(Application& app)
    : mApp(app)
    , mPublishersSize(
          app.getMetrics().NewCounter({"history", "memory", "publishers"}))
    , mPendingSnapsSize(
          app.getMetrics().NewCounter({"history", "memory", "pending-snaps"}))
    , mRecheckRunningMergeTimer(app)
{
}

uint32_t
PublishStateMachine::maxQueuedSnapshotLedger() const
{
    if (mPendingSnaps.empty())
    {
        return 0;
    }
    return mPendingSnaps.back()->mLocalState.currentLedger;
}

uint32_t
PublishStateMachine::minQueuedSnapshotLedger() const
{
    if (mPendingSnaps.empty())
    {
        return 0;
    }
    return mPendingSnaps.front()->mLocalState.currentLedger;
}

size_t
PublishStateMachine::publishQueueLength() const
{
    return mPendingSnaps.size();
}

bool
PublishStateMachine::queueSnapshot(SnapshotPtr snap)
{
    bool delayed = !mPendingSnaps.empty();
    mPendingSnaps.push_back(snap);
    mPendingSnapsSize.set_count(mPendingSnaps.size());
    if (delayed)
    {
        CLOG(WARNING, "History") << "Snapshot queued while already publishing";
    }
    else
    {
        writeNextSnapshot();
    }
    return delayed;
}

std::shared_ptr<StateSnapshot>
PublishStateMachine::takeSnapshot(Application& app,
                                  HistoryArchiveState const& state)
{
    // Capture local state and _all_ the local buckets at this instant; these
    // may be expired from the bucketlist while the subsequent put-callbacks are
    // running but the buckets are immutable and we hold shared_ptrs to them
    // here, include those in the callbacks themselves.
    return std::make_shared<StateSnapshot>(app, state);
}

void
PublishStateMachine::writeNextSnapshot()
{
    // Once we've taken a (synchronous) snapshot of the buckets and db, we then
    // run writeHistoryBlocks() to get the tx and ledger history files written
    // out from the db. This may run synchronously (if we're not using a
    // thread-pool-friendly db backend) or asynchronously on the worker pool if
    // we're on, say, postgres). In either case, when complete it will call back
    // to snapshotWritten(), at which point we can begin the actual publishing
    // work.

    if (mPendingSnaps.empty())
        return;

    auto snap = mPendingSnaps.front();

    snap->mLocalState.resolveAnyReadyFutures();
    snap->makeLiveAndRetainBuckets();

    bool readyToWrite = true;

    if (mApp.getLedgerManager().getState() != LedgerManager::LM_SYNCED_STATE)
    {
        readyToWrite = false;
        CLOG(WARNING, "History")
            << "Queued snapshot awaiting ledgermanager sync";
    }

    else if (!snap->mLocalState.futuresAllResolved())
    {
        readyToWrite = false;
        CLOG(WARNING, "History")
            << "Queued snapshot still awaiting running merges";
    }

    if (!readyToWrite)
    {
        mRecheckRunningMergeTimer.expires_from_now(std::chrono::seconds(2));
        mRecheckRunningMergeTimer.async_wait([this](asio::error_code const& ec)
                                             {
                                                 if (!ec)
                                                 {
                                                     this->writeNextSnapshot();
                                                 }
                                             });
        return;
    }

    snap->writeHistoryBlocksWithRetry();
}

void
PublishStateMachine::snapshotWritten(asio::error_code const& ec)
{
    auto snap = mPendingSnaps.front();

    if (ec)
    {
        CLOG(WARNING, "History") << "Failed to snapshot state";
        finishOne(false);
        return;
    }
    CLOG(DEBUG, "History") << "Publishing snapshot of ledger "
                           << snap->mLocalState.currentLedger;

    // Iterate over writable archives instantiating an ArchivePublisher for them
    // with a callback that returns to the PublishStateMachine and possibly
    // fires its own callback when they are all done.
    auto const& hist = mApp.getConfig().HISTORY;
    std::vector<std::shared_ptr<HistoryArchive>> writableArchives;
    for (auto const& pair : hist)
    {
        if (pair.second->hasGetCmd() && pair.second->hasPutCmd())
        {
            auto p = std::make_shared<ArchivePublisher>(
                mApp, *this, pair.second, snap);
            mPublishers.push_back(p);
            mPublishersSize.set_count(mPublishers.size());
            p->enterBeginState();
        }
    }
}

void
PublishStateMachine::snapshotPublished()
{
    size_t nDone = 0, nFail = 0;
    for (auto const& p : mPublishers)
    {
        if (p->isDone())
        {
            ++nDone;
            if (p->failed())
            {
                ++nFail;
            }
        }
    }

    CLOG(DEBUG, "History") << "Completed publish to archive, "
                           << (mPublishers.size() - nDone) << " remain";

    if (nDone == mPublishers.size())
    {
        if (nFail != 0)
        {
            CLOG(WARNING, "History") << "Failed to publish " << nFail
                                     << "archives, will retry later.";
        }
        finishOne(nFail == 0);
    }
}

void
PublishStateMachine::finishOne(bool success)
{
    assert(!mPendingSnaps.empty());
    auto seq = mPendingSnaps.front()->mLocalState.currentLedger;
    mApp.getHistoryManager().historyPublished(seq, success);
    mPendingSnaps.pop_front();
    mPendingSnapsSize.set_count(mPendingSnaps.size());
    writeNextSnapshot();
}
}
