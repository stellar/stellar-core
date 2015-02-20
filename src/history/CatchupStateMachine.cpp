// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "history/CatchupStateMachine.h"
#include "history/HistoryMaster.h"

#include "crypto/Hex.h"
#include "main/Application.h"
#include "main/Config.h"
#include "ledger/LedgerDelta.h"
#include "database/Database.h"
#include "util/Logging.h"
#include "util/XDRStream.h"

#include <random>

namespace stellar
{

const size_t
CatchupStateMachine::kRetryLimit = 16;

CatchupStateMachine::CatchupStateMachine(Application& app)
    : mApp(app)
    , mState(CATCHUP_RETRYING)
    , mRetryCount(0)
    , mRetryTimer(app.getClock())
{
    // We start up in CATCHUP_RETRYING as that's the only valid
    // named pre-state for CATCHUP_BEGIN.
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
                                     std::string const& basename,
                                     FileCatchupState newGoodState)
{
    FileCatchupState newState = newGoodState;
    if (ec)
    {
        CLOG(INFO, "History") << "Catchup action failed on " << basename;
        newState = FILE_CATCHUP_FAILED;
    }
    mFileStates[basename] = newState;
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
    for (auto& f : mFileStates)
    {
        std::string basename = f.first;
        std::string filename = hm.localFilename(basename);
        minimumState = std::min(f.second, minimumState);
        switch (f.second)
        {
        case FILE_CATCHUP_FAILED:
            break;

        case FILE_CATCHUP_NEEDED:
            f.second = FILE_CATCHUP_DOWNLOADING;
            CLOG(INFO, "History") << "Downloading " << basename;
            hm.getFile(
                mArchive,
                basename, filename,
                [this, basename](asio::error_code const& ec)
                {
                    this->fileStateChange(ec, basename, FILE_CATCHUP_DOWNLOADED);
                });
            break;

        case FILE_CATCHUP_DOWNLOADING:
            break;

        case FILE_CATCHUP_DOWNLOADED:
            f.second = FILE_CATCHUP_DECOMPRESSING;
            CLOG(INFO, "History") << "Decompressing " << basename;
            hm.decompress(
                filename,
                [this, basename](asio::error_code const& ec)
                {
                    this->fileStateChange(ec, basename, FILE_CATCHUP_DECOMPRESSED);
                });
            break;

        case FILE_CATCHUP_DECOMPRESSING:
            break;

        case FILE_CATCHUP_DECOMPRESSED:
            f.second = FILE_CATCHUP_VERIFYING;

            // Note: verification here does not guarantee that the data is
            // _trustworthy_, merely that it's the data we were expecting
            // by-hash-name, not damaged in transport. In other words this check
            // is just to save us wasting time applying XDR blobs that are
            // corrupt. Trusting that hash name is a whole other issue, the data
            // might still be full of lies and attacks at the ledger-level.

            CLOG(INFO, "History") << "Verifying " << basename;
            hm.verifyHash(
                filename,
                hexToBin256(HistoryMaster::bucketHexHash(basename)),
                [this, basename](asio::error_code const& ec)
                {
                    this->fileStateChange(ec, basename, FILE_CATCHUP_VERIFIED);
                });
            break;

        case FILE_CATCHUP_VERIFYING:
            break;

        case FILE_CATCHUP_VERIFIED:
            break;
        }
    }

    if (minimumState == FILE_CATCHUP_FAILED)
    {
        CLOG(INFO, "History") << "Some fetches failed, retrying";
        enterRetryingState();
    }
    else if (minimumState == FILE_CATCHUP_VERIFIED)
    {
        CLOG(INFO, "History") << "All fetches verified, applying";
        enterApplyingState();
    }
    else
    {
        CLOG(INFO, "History") << "Some fetches still in progress";
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
    auto& hm = mApp.getHistoryMaster();
    for (auto& f : mFileStates)
    {
        std::string filename = hm.localFilename(f.first);
        if (f.second == FILE_CATCHUP_FAILED)
        {
            std::remove(filename.c_str());
            CLOG(INFO, "History")
                << "Retrying fetch for " << f.first
                << " from archive '" << mArchive->getName() << "'";
            f.second = FILE_CATCHUP_NEEDED;
        }
    }

    // Then make sure all the files we _want_ are either present
    // or queued to be requested.
    for (auto const& bucket : has.currentBuckets)
    {
        std::string basenames[] = { HistoryMaster::bucketBasename(bucket.snap),
                                    HistoryMaster::bucketBasename(bucket.curr) };
        for (auto const& b : basenames)
        {
            if (b.empty())
            {
                continue;
            }
            if (mFileStates.find(b) == mFileStates.end())
            {
                CLOG(INFO, "History")
                    << "Starting fetch for " << b
                    << " from archive '" << mArchive->getName() << "'";
                mFileStates[b] = FILE_CATCHUP_NEEDED;
            }
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
                CLOG(INFO, "History")
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
    auto& hm = mApp.getHistoryMaster();
    auto& db = mApp.getDatabase();
    CLFEntry entry;
    auto& sess = db.getSession();
    soci::transaction tx(sess);
    LedgerDelta delta;
    for (auto const& bucket : mArchiveState.currentBuckets)
    {
        std::string basenames[] = { HistoryMaster::bucketBasename(bucket.snap),
                                    HistoryMaster::bucketBasename(bucket.curr) };

        for (auto const& b : basenames)
        {
            if (b.empty())
            {
                continue;
            }
            assert(mFileStates[b] == FILE_CATCHUP_VERIFIED);
            std::string filename = hm.localFilename(b);
            XDRInputFileStream in;
            in.open(filename);
            while (in)
            {
                in.readOne(entry);
                if (entry.entry.type() == LIVEENTRY)
                {
                    EntryFrame::pointer ep = EntryFrame::FromXDR(entry.entry.liveEntry());
                    ep->storeAddOrChange(delta, db);
                }
                else
                {
                    EntryFrame::storeDelete(delta, db, entry.entry.deadEntry());
                }
            }
        }
    }
}

void CatchupStateMachine::enterEndState()
{
}


}
