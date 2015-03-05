// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "history/CatchupStateMachine.h"
#include "history/HistoryMaster.h"
#include "history/FileTransferInfo.h"

#include "clf/CLFMaster.h"
#include "clf/BucketList.h"
#include "crypto/Hex.h"
#include "main/Application.h"
#include "main/Config.h"
#include "ledger/LedgerDelta.h"
#include "database/Database.h"
#include "util/Logging.h"
#include "util/XDRStream.h"

#include <random>
#include <memory>

namespace stellar
{

const size_t
CatchupStateMachine::kRetryLimit = 16;

typedef FileTransferInfo<FileCatchupState> FileCatchupInfo;

CatchupStateMachine::CatchupStateMachine(Application& app,
                                         std::function<void(asio::error_code const&)> handler)
    : mApp(app)
    , mEndHandler(handler)
    , mState(CATCHUP_RETRYING)
    , mRetryCount(0)
    , mRetryTimer(app)
    , mDownloadDir(app.getTmpDirMaster().tmpDir("catchup"))
{
    // We start up in CATCHUP_RETRYING as that's the only valid
    // named pre-state for CATCHUP_BEGIN.
    mLocalState = app.getHistoryMaster().getCurrentHistoryArchiveState();
    enterBeginState();
}

/**
 * Select any readable history archive. If there are more than one,
 * select one at random.
 */
std::shared_ptr<HistoryArchive>
CatchupStateMachine::selectRandomReadableHistoryArchive()
{
    std::vector<std::pair<std::string,
                          std::shared_ptr<HistoryArchive>>> archives;

    for (auto const& pair : mApp.getConfig().HISTORY)
    {
        if (pair.second->hasGetCmd())
        {
            archives.push_back(pair);
        }
    }

    if (archives.size() == 0)
    {
        throw std::runtime_error("No readable history archive to catch up from.");
    }
    else if (archives.size() == 1)
    {
        CLOG(INFO, "History")
            << "Catching up via sole readable history archive '"
            << archives[0].first << "'";
        return archives[0].second;
    }
    else
    {
        std::default_random_engine gen;
        std::uniform_int_distribution<size_t> dist(0, archives.size() - 1);
        size_t i = dist(gen);
        CLOG(INFO, "History")
            << "Catching up via readable history archive #" << i << ", '"
            << archives[i].first << "'";
        return archives[i].second;
    }
}


/**
 * Select a random history archive and read its state, passing the
 * state to enterAnchoredState() when it's retrieved, restarting
 * (potentially with a new archive) if there's any failure.
 */
void
CatchupStateMachine::enterBeginState()
{
    assert(mState == CATCHUP_RETRYING);
    mRetryCount = 0;
    mState = CATCHUP_BEGIN;
    CLOG(INFO, "History") << "Catchup BEGIN";

    mArchive = selectRandomReadableHistoryArchive();
    mArchive->getState(
        mApp,
        [this](asio::error_code const& ec,
               HistoryArchiveState const& has)
        {
            if (ec)
            {
                CLOG(WARNING, "History")
                    << "Catchup failed to retrieve state from history archive '"
                    << this->mArchive->getName() << "', restarting catchup";
                this->enterRetryingState();
            }
            else
            {
                this->enterAnchoredState(has);
            }
        });
}


/**
 * If `ec` is an error, set the state for `basename` to FILE_FAILED, otherwise
 * set it to `newGoodState`. In either case, re-enter FETCHING state.
 */
void
CatchupStateMachine::fileStateChange(asio::error_code const& ec,
                                     std::string const& name,
                                     FileCatchupState newGoodState)
{
    FileCatchupState newState = newGoodState;
    if (ec)
    {
        CLOG(WARNING, "History") << "Catchup action failed on " << name;
        newState = FILE_CATCHUP_FAILED;
    }
    mFileInfos[name]->setState(newState);
    enterFetchingState();
}

/**
 * Attempt to step the state machine through a file-state-driven state
 * transition. The state machine advances only when all the files have
 * reached the next step in their lifecycle.
 */
void
CatchupStateMachine::enterFetchingState()
{
    assert(mState == CATCHUP_ANCHORED || mState == CATCHUP_FETCHING);
    mState = CATCHUP_FETCHING;

    FileCatchupState minimumState = FILE_CATCHUP_VERIFIED;
    auto& hm = mApp.getHistoryMaster();
    for (auto& pair : mFileInfos)
    {
        auto fi = pair.second;
        auto name = fi->baseName_nogz();

        std::string hashname;
        uint256 hash;
        if (fi->getBucketHashName(hashname))
        {
            hash = hexToBin256(hashname);
        }

        switch (fi->getState())
        {
        case FILE_CATCHUP_FAILED:
            break;

        case FILE_CATCHUP_NEEDED:
        {
            std::shared_ptr<Bucket> b;
            if (!hashname.empty())
            {
                b = mApp.getCLFMaster().getBucketByHash(hash);
            }
            if (b)
            {
                // If for some reason this bucket exists and is live in the CLF,
                // just grab a copy of it.
                CLOG(INFO, "History") << "Existing bucket found in CLF: " << hashname;
                mBuckets[hashname] = b;
                fi->setState(FILE_CATCHUP_VERIFIED);
            }
            else
            {
                fi->setState(FILE_CATCHUP_DOWNLOADING);
                CLOG(INFO, "History") << "Downloading " << name;
                hm.getFile(
                    mArchive,
                    fi->baseName_gz(), fi->localPath_gz(),
                    [this, name](asio::error_code const& ec)
                    {
                        this->fileStateChange(ec, name, FILE_CATCHUP_DOWNLOADED);
                    });
            }
        }
        break;

        case FILE_CATCHUP_DOWNLOADING:
            break;

        case FILE_CATCHUP_DOWNLOADED:
            fi->setState(FILE_CATCHUP_DECOMPRESSING);
            CLOG(INFO, "History") << "Decompressing " << fi->baseName_gz();
            hm.decompress(
                fi->localPath_gz(),
                [this, name](asio::error_code const& ec)
                {
                    this->fileStateChange(ec, name, FILE_CATCHUP_DECOMPRESSED);
                });
            break;

        case FILE_CATCHUP_DECOMPRESSING:
            break;

        case FILE_CATCHUP_DECOMPRESSED:
            fi->setState(FILE_CATCHUP_VERIFYING);

            // Note: verification here does not guarantee that the data is
            // _trustworthy_, merely that it's the data we were expecting
            // by-hash-name, not damaged in transport. In other words this check
            // is just to save us wasting time applying XDR blobs that are
            // corrupt. Trusting that hash name is a whole other issue, the data
            // might still be full of lies and attacks at the ledger-level.

            if (hashname.empty())
            {
                CLOG(INFO, "History") << "Not verifying " << name << ", no hash";
                fi->setState(FILE_CATCHUP_VERIFIED);
            }
            else
            {
                CLOG(INFO, "History") << "Verifying " << name;
                auto filename = fi->localPath_nogz();
                hm.verifyHash(
                    filename, hash,
                    [this, name, filename, hashname, hash](asio::error_code const& ec)
                    {
                        if (!ec)
                        {
                            auto b = this->mApp.getCLFMaster().adoptFileAsBucket(filename, hash);
                            this->mBuckets[hashname] = b;
                        }
                        this->fileStateChange(ec, name, FILE_CATCHUP_VERIFIED);
                    });
            }
            break;

        case FILE_CATCHUP_VERIFYING:
            break;

        case FILE_CATCHUP_VERIFIED:
            break;
        }

        minimumState = std::min(fi->getState(), minimumState);
    }

    if (minimumState == FILE_CATCHUP_FAILED)
    {
        CLOG(WARNING, "History") << "Some fetches failed, retrying";
        enterRetryingState();
    }
    else if (minimumState == FILE_CATCHUP_VERIFIED)
    {
        CLOG(INFO, "History") << "All fetches verified, applying";
        enterApplyingState();
    }
    else
    {
        CLOG(DEBUG, "History") << "Some fetches still in progress";
        // Do nothing here; in-progress states have callbacks set up already
        // which will fire when they complete.
    }
}

void
CatchupStateMachine::enterAnchoredState(HistoryArchiveState const& has)
{
    assert(mState == CATCHUP_BEGIN);
    mState = CATCHUP_ANCHORED;
    mArchiveState = has;

    CLOG(INFO, "History") << "Catchup ANCHORED, anchor ledger = "
                          << mArchiveState.currentLedger;

    // First clear out any previous failed files, so we retry them.
    for (auto& pair : mFileInfos)
    {
        auto fi = pair.second;

        if (fi->getState() == FILE_CATCHUP_FAILED)
        {
            auto filename_nogz = fi->localPath_nogz();
            auto filename_gz = fi->localPath_gz();

            std::remove(filename_nogz.c_str());
            std::remove(filename_gz.c_str());
            CLOG(INFO, "History")
                << "Retrying fetch for " << fi->baseName_gz()
                << " from archive '" << mArchive->getName() << "'";
            fi->setState(FILE_CATCHUP_NEEDED);
        }
    }

    // Then make sure all the files we _want_ are either present
    // or queued to be requested.
    std::vector<std::string> toFetch = mArchiveState.differingBuckets(mLocalState);
    for (auto const& h : toFetch)
    {
        if (mFileInfos.find(h) == mFileInfos.end())
        {
            auto fi = std::make_shared<FileCatchupInfo>(
                FILE_CATCHUP_NEEDED, mDownloadDir,
                HISTORY_FILE_TYPE_BUCKET, h);
            CLOG(INFO, "History")
                << "Starting fetch for " << fi->baseName_gz()
                << " from archive '" << mArchive->getName() << "'";
            mFileInfos[fi->baseName_nogz()] = fi;
        }
    }

    enterFetchingState();
}

void
CatchupStateMachine::enterRetryingState()
{
    assert(mState == CATCHUP_ANCHORED || mState == CATCHUP_FETCHING || mState == CATCHUP_APPLYING);
    mState = CATCHUP_RETRYING;
    mRetryTimer.expires_from_now(std::chrono::seconds(2));
    mRetryTimer.async_wait(
        [this](asio::error_code const& ec)
        {
            if (this->mRetryCount++ > kRetryLimit)
            {
                CLOG(WARNING, "History")
                    << "Retry count " << kRetryLimit << " exceeded, restarting catchup";
                this->enterBeginState();
            }
            else
            {
                CLOG(INFO, "History")
                    << "Retrying catchup with archive '"
                    << this->mArchive->getName() << "'";
                this->enterAnchoredState(this->mArchiveState);
            }
        });
}

void CatchupStateMachine::enterApplyingState()
{
    assert(mState == CATCHUP_FETCHING);
    mState = CATCHUP_APPLYING;
    auto& db = mApp.getDatabase();
    CLFEntry entry;
    auto& sess = db.getSession();
    soci::transaction tx(sess);
    LedgerDelta delta;

    auto& bl = mApp.getCLFMaster().getBucketList();

    auto n = BucketList::kNumLevels;
    bool applying = false;

    // Apply in reverse order, oldest bucket to new. Once we
    // apply one bucket, apply all buckets newer as well.
    for (auto i = mArchiveState.currentBuckets.rbegin();
         i != mArchiveState.currentBuckets.rend(); i++)
    {
        --n;
        BucketLevel& existingLevel = bl.getLevel(n);

        if (applying ||
            i->snap != binToHex(existingLevel.getSnap()->getHash()))
        {
            std::shared_ptr<Bucket> b;
            if (i->snap.find_first_not_of('0') == std::string::npos)
            {
                b = std::make_shared<Bucket>();
            }
            else
            {
                b = mBuckets[i->snap];
            }
            assert(b);
            b->apply(db);
            existingLevel.setSnap(b);
            applying = true;
        }

        if (applying ||
            i->curr != binToHex(existingLevel.getCurr()->getHash()))
        {
            std::shared_ptr<Bucket> b;
            if (i->curr.find_first_not_of('0') == std::string::npos)
            {
                b = std::make_shared<Bucket>();
            }
            else
            {
                b = mBuckets[i->curr];
            }
            assert(b);
            b->apply(db);
            existingLevel.setCurr(b);
            applying = true;
        }
    }
    enterEndState();
}

void CatchupStateMachine::enterEndState()
{
    assert(mState == CATCHUP_APPLYING);
    mState = CATCHUP_END;
    CLOG(DEBUG, "History")
        << "Completed catchup from '" << mArchive->getName() << "'";
    mEndHandler(mError);
}


}
