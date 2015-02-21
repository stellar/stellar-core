// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "clf/Bucket.h"
#include "clf/BucketList.h"
#include "clf/CLFMaster.h"
#include "crypto/Hex.h"
#include "history/HistoryMaster.h"
#include "history/PublishStateMachine.h"
#include "main/Application.h"
#include "main/Config.h"
#include "util/Logging.h"

#include <set>

namespace stellar
{

const size_t
ArchivePublisher::kRetryLimit = 16;

ArchivePublisher::ArchivePublisher(Application& app,
                                   std::function<void(asio::error_code const&)> handler,
                                   std::shared_ptr<HistoryArchive> archive,
                                   std::vector<std::pair<std::shared_ptr<Bucket>,
                                                         std::shared_ptr<Bucket>>> const& localBuckets)
    : mApp(app)
    , mEndHandler(handler)
    , mState(PUBLISH_RETRYING)
    , mRetryCount(0)
    , mRetryTimer(app.getClock())
    , mArchive(archive)
    , mBucketsToPublish(localBuckets)
{
    enterBeginState();
}

void
ArchivePublisher::enterRetryingState()
{
    assert(mState == PUBLISH_BEGIN || mState == PUBLISH_SENDING || mState == PUBLISH_COMMITTING);
    mState = PUBLISH_RETRYING;
    mRetryTimer.expires_from_now(std::chrono::seconds(2));
    mRetryTimer.async_wait(
        [this](asio::error_code const& ec)
        {
            if (this->mRetryCount++ > kRetryLimit)
            {
                CLOG(WARNING, "History")
                    << "Retry count " << kRetryLimit
                    << " exceeded, abandonning  publish-attempt on archive '"
                    << this->mArchive->getName() << "'";
                this->enterEndState();
            }
            else
            {
                CLOG(INFO, "History")
                    << "Retrying publish to archive '"
                    << this->mArchive->getName() << "'";
                this->enterBeginState();
            }
        });
}

void
ArchivePublisher::enterBeginState()
{
    assert(mState == PUBLISH_RETRYING);
    mState = PUBLISH_BEGIN;
    mArchive->getState(
        mApp,
        [this](asio::error_code const& ec,
               HistoryArchiveState const& has)
        {
            if (ec)
            {
                CLOG(WARNING, "History")
                    << "Publisher failed to retrieve state from history archive '"
                    << this->mArchive->getName() << "', restarting publish";
                this->enterRetryingState();
            }
            else
            {
                this->enterObservedState(has);
            }
        });
}

void
ArchivePublisher::enterObservedState(HistoryArchiveState const& has)
{
    assert(mState == PUBLISH_BEGIN);
    mState = PUBLISH_OBSERVED;

    mObservedArchiveState = has;
    mIntendedArchiveState = has;

    std::set<std::string> bucketsInArchive;
    for (auto const& bucket : has.currentBuckets)
    {
        bucketsInArchive.insert(bucket.curr);
        bucketsInArchive.insert(bucket.snap);
    }
    size_t b = 0;
    for (auto const& pair : mBucketsToPublish)
    {
        mIntendedArchiveState.currentBuckets.at(b).curr = binToHex(pair.first->getHash());
        mIntendedArchiveState.currentBuckets.at(b).snap = binToHex(pair.second->getHash());

        size_t n = b++ * 2;

        std::shared_ptr<Bucket> p[2] = { pair.first, pair.second };
        for (auto const& b : p)
        {
            auto hash = binToHex(b->getHash());
            if (bucketsInArchive.find(hash) ==
                bucketsInArchive.end())
            {
                auto i = mFileStates.find(hash);
                if (i == mFileStates.end() ||
                    i->second == FILE_PUBLISH_FAILED)
                {
                    CLOG(DEBUG, "History")
                        << "Queueing bucket " << n
                        << ": " << hexAbbrev(b->getHash())
                        << " to send to archive '" << mArchive->getName() << "'";
                    mFileStates[hash] = FILE_PUBLISH_NEEDED;
                }
                else
                {
                    CLOG(DEBUG, "History")
                        << "Not queueing bucket " << n
                        << ": " << hexAbbrev(b->getHash())
                        << " to send to archive '" << mArchive->getName()
                        << "'; bucket already queued";
                }
            }
            else
            {
                CLOG(DEBUG, "History")
                    << "Not queueing bucket " << n
                    << ": " << hexAbbrev(b->getHash())
                    << " to send to archive '" << mArchive->getName()
                    << "', which already has it";
            }
            ++n;
        }
    }
    enterSendingState();
}


/**
 * If `ec` is an error, set the state for `basename` to FILE_FAILED, otherwise
 * set it to `newGoodState`. In either case, re-enter FETCHING state.
 */
void
ArchivePublisher::fileStateChange(asio::error_code const& ec,
                                  std::string const& hashname,
                                  FilePublishState newGoodState)
{
    FilePublishState newState = newGoodState;
    if (ec)
    {
        CLOG(WARNING, "History") << "Publish action failed on " << hashname;
        newState = FILE_PUBLISH_FAILED;
        mError = ec;
    }
    mFileStates[hashname] = newState;
    enterSendingState();
}

void
ArchivePublisher::enterSendingState()
{
    assert(mState == PUBLISH_OBSERVED || mState == PUBLISH_SENDING);
    mState = PUBLISH_SENDING;

    FilePublishState minimumState = FILE_PUBLISH_UPLOADED;
    auto& hm = mApp.getHistoryMaster();
    for (auto& f : mFileStates)
    {
        std::string hashname = f.first;
        std::string basename = hm.bucketBasename(hashname);
        std::string filename = mApp.getCLFMaster().getBucketDir() + "/" + basename;

        std::string basename_gz = basename + ".gz";
        std::string filename_gz = filename + ".gz";

        minimumState = std::min(f.second, minimumState);
        switch (f.second)
        {
        case FILE_PUBLISH_FAILED:
            break;

        case FILE_PUBLISH_NEEDED:
            f.second = FILE_PUBLISH_COMPRESSING;
            CLOG(DEBUG, "History") << "Compressing " << basename;
            hm.compress(
                filename,
                [this, hashname](asio::error_code const& ec)
                {
                    this->fileStateChange(ec, hashname, FILE_PUBLISH_COMPRESSED);
                }, true);
            break;


        case FILE_PUBLISH_COMPRESSING:
            break;

        case FILE_PUBLISH_COMPRESSED:
            f.second = FILE_PUBLISH_UPLOADING;
            CLOG(INFO, "History") << "Publishing " << basename_gz;
            hm.putFile(
                mArchive,
                filename_gz,
                basename_gz,
                [this, hashname](asio::error_code const& ec)
                {
                    this->fileStateChange(ec, hashname, FILE_PUBLISH_UPLOADED);
                });
            break;

        case FILE_PUBLISH_UPLOADING:
            break;

        case FILE_PUBLISH_UPLOADED:
            std::remove(filename_gz.c_str());
            break;
        }
    }

    if (minimumState == FILE_PUBLISH_FAILED)
    {
        CLOG(WARNING, "History") << "Some file-puts failed, retrying";
        enterRetryingState();
    }
    else if (minimumState == FILE_PUBLISH_UPLOADED)
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
    mArchive->putState(
        mApp,
        mIntendedArchiveState,
        [this](asio::error_code const& ec)
        {
            if (ec)
            {
                CLOG(WARNING, "History")
                    << "Publisher failed to update state in history archive '"
                    << this->mArchive->getName() << "', restarting publish";
                this->enterRetryingState();
            }
            else
            {
                this->enterEndState();
            }
        });
}

void
ArchivePublisher::enterEndState()
{
    CLOG(DEBUG, "History") << "Finished publishing to archive '"
                           << this->mArchive->getName() << "'";
    mState = PUBLISH_END;
    mEndHandler(mError);
}


bool
ArchivePublisher::isDone() const
{
    return mState == PUBLISH_END;
}

PublishStateMachine::PublishStateMachine(Application& app,
                                         std::function<void(asio::error_code const&)> handler)
    : mApp(app)
    , mEndHandler(handler)
{
    BucketList& buckets = mApp.getCLFMaster().getBucketList();

    // Capture the bucket pairs at this instant; these may be expired from the
    // bucketlist while the subsequent put-callbacks are running but the buckets
    // are immutable and we hold shared_ptrs to them here, include those in the
    // callbacks themselves.
    std::vector<std::pair<std::shared_ptr<Bucket>,
                          std::shared_ptr<Bucket>>> localBuckets;
    for (size_t i = 0; i < buckets.numLevels(); ++i)
    {
        auto const& level = buckets.getLevel(i);
        localBuckets.push_back(std::make_pair(level.getCurr(), level.getSnap()));
    }

    // Iterate over writable archives instantiating an ArchivePublisher for them
    // with a callback that returns to the PublishStateMachine and possibly
    // fires its own callback when they are all done.
    auto const& hist = mApp.getConfig().HISTORY;
    std::vector<std::shared_ptr<HistoryArchive>> writableArchives;
    for (auto const& pair : hist)
    {
        if (pair.second->hasGetCmd() &&
            pair.second->hasPutCmd())
        {
            auto p = std::make_shared<ArchivePublisher>(
                mApp,
                [this](asio::error_code const& ec)
                {
                    this->archiveComplete(ec);
                },
                pair.second,
                localBuckets);
            mPublishers.push_back(p);
        }
    }
}

void
PublishStateMachine::archiveComplete(asio::error_code const& ec)
{
    if (ec)
    {
        mError = ec;
    }
    mPublishers.erase(
        std::remove_if(
            mPublishers.begin(),
            mPublishers.end(),
            [](std::shared_ptr<ArchivePublisher> p)
            {
                return p->isDone();
            }));
    CLOG(DEBUG, "History") << "Completed publish to archive, "
                           << mPublishers.size() << " remain";
    if (mPublishers.empty())
    {
        mEndHandler(mError);
    }
}


}
