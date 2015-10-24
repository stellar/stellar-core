// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "history/CatchupStateMachine.h"
#include "history/HistoryManager.h"
#include "history/FileTransferInfo.h"

#include "bucket/BucketManager.h"
#include "bucket/BucketList.h"
#include "crypto/Hex.h"
#include "main/Application.h"
#include "main/Config.h"
#include "database/Database.h"
#include "herder/TxSetFrame.h"
#include "herder/HerderImpl.h"
#include "ledger/LedgerDelta.h"
#include "ledger/LedgerManager.h"
#include "transactions/TransactionFrame.h"
#include "herder/LedgerCloseData.h"
#include "util/Logging.h"
#include "util/XDRStream.h"
#include "xdrpp/printer.h"
#include "util/Math.h"

#include <random>
#include <memory>

namespace stellar
{

const size_t CatchupStateMachine::kRetryLimit = 16;

const std::chrono::seconds CatchupStateMachine::SLEEP_SECONDS_PER_LEDGER =
    Herder::EXP_LEDGER_TIMESPAN_SECONDS + std::chrono::seconds(1);

// Helper struct that encapsulates the state of ongoing incremental
// application of either buckets or headers.
struct CatchupStateMachine::ApplyState
    : std::enable_shared_from_this<ApplyState>
{
    // Variables for CATCHUP_COMPLETE application
    uint32_t mCheckpointNumber;

    // Variables for CATCHUP_MINIMAL application
    size_t mBucketLevel{BucketList::kNumLevels - 1};
    bool mApplyingBuckets{false};

    ApplyState(Application& app)
    {
    }
};

CatchupStateMachine::CatchupStateMachine(
    Application& app, uint32_t initLedger, HistoryManager::CatchupMode mode,
    HistoryArchiveState localState,
    std::function<void(asio::error_code const& ec,
                       HistoryManager::CatchupMode mode,
                       LedgerHeaderHistoryEntry const& lastClosed)> handler)
    : mApp(app)
    , mInitLedger(initLedger)
    , mNextLedger(app.getHistoryManager().nextCheckpointLedger(initLedger))
    , mMode(mode)
    , mEndHandler(handler)
    , mState(CATCHUP_RETRYING)
    , mRetryCount(0)
    , mRetryTimer(app)
    , mDownloadDir(app.getTmpDirManager().tmpDir("catchup"))
    , mLocalState(localState)
{
    mLocalState.resolveAllFutures();
}

void
CatchupStateMachine::begin()
{
    // We start up in CATCHUP_RETRYING as that's the only valid
    // named pre-state for CATCHUP_BEGIN.
    //
    // This also has to be its own function so that there's a stable
    // shared_ptr for self from which to call shared_from_this; can't
    // call it during the constructor.
    enterBeginState();
}

void
CatchupStateMachine::logAndUpdateStatus(bool contiguous)
{
    std::stringstream stateStr;
    stateStr << "Catchup mode";

    switch (mMode)
    {
    case HistoryManager::CATCHUP_MINIMAL:
        stateStr << " 'minimal'";
        break;
    case HistoryManager::CATCHUP_COMPLETE:
        stateStr << " 'complete'";
        break;
    case HistoryManager::CATCHUP_BUCKET_REPAIR:
        stateStr << " 'bucket-repair'";
        break;
    default:
        break;
    }

    switch (mState)
    {
    case CATCHUP_BEGIN:
        stateStr << " beginning";
        break;

    case CATCHUP_RETRYING:
    {
        uint64 retry = VirtualClock::to_time_t(mRetryTimer.expiry_time());
        uint64 now = mApp.timeNow();
        uint64 eta = now > retry ? 0 : retry - now;
        stateStr << " awaiting checkpoint"
                 << " (ETA: " << eta << " seconds)";
    }
    break;

    case CATCHUP_ANCHORED:
        stateStr << " anchored";
        break;

    case CATCHUP_FETCHING:
    {
        size_t nFiles = mFileInfos.size();
        size_t nDownloaded = 0;
        for (auto& pair : mFileInfos)
        {
            if (pair.second->getState() >= FILE_CATCHUP_DOWNLOADED)
            {
                ++nDownloaded;
            }
        }
        stateStr << ", downloaded " << nDownloaded << "/" << nFiles << " files";
    }
    break;

    case CATCHUP_VERIFYING:
        if (mMode == HistoryManager::CATCHUP_COMPLETE)
        {
            // FIXME: give a progress count here, maybe? It usually
            // runs very quickly, even on huge history. Like disk-speed.
            stateStr << ", verifying " << mHeaderInfos.size()
                     << "-ledger history chain";
        }
        else
        {
            stateStr << ", verifying buckets";
        }
        break;

    case CATCHUP_APPLYING:
        if (mMode == HistoryManager::CATCHUP_COMPLETE)
        {
            assert(mApplyState);
            stateStr << ", applying ledger " << mApplyState->mCheckpointNumber
                     << "/"
                     << (mHeaderInfos.empty() ? 0
                                              : mHeaderInfos.rbegin()->first);
        }
        else if (mMode == HistoryManager::CATCHUP_MINIMAL)
        {
            assert(mApplyState);
            stateStr << ", applying bucket level " << mApplyState->mBucketLevel;
        }
        break;

    case CATCHUP_END:
        stateStr << ", finished";
        break;

    default:
        break;
    }

    if (!contiguous)
    {
        stateStr << " (discontiguous, will restart)";
    }

    size_t nQueuedToPublish = mApp.getHistoryManager().publishQueueLength();
    if (nQueuedToPublish != 0)
    {
        stateStr << ", queued checkpoints: " << nQueuedToPublish;
    }

    CLOG(INFO, "History") << stateStr.str();
    mApp.setExtraStateInfo(stateStr.str());
}

/**
 * Select any readable history archive. If there are more than one,
 * select one at random.
 */
std::shared_ptr<HistoryArchive>
CatchupStateMachine::selectRandomReadableHistoryArchive()
{
    std::vector<std::pair<std::string, std::shared_ptr<HistoryArchive>>>
        archives;

    // First try for archives that _only_ have a get command; they're
    // archives we're explicitly not publishing to, so likely ones we want.
    for (auto const& pair : mApp.getConfig().HISTORY)
    {
        if (pair.second->hasGetCmd() && !pair.second->hasPutCmd())
        {
            archives.push_back(pair);
        }
    }

    // If we have none of those, accept those with get+put
    if (archives.size() == 0)
    {
        for (auto const& pair : mApp.getConfig().HISTORY)
        {
            if (pair.second->hasGetCmd() && pair.second->hasPutCmd())
            {
                archives.push_back(pair);
            }
        }
    }

    if (archives.size() == 0)
    {
        throw std::runtime_error(
            "No GET-enabled history archive in config, can't start catchup.");
    }
    else if (archives.size() == 1)
    {
        CLOG(DEBUG, "History")
            << "Catching up via sole readable history archive '"
            << archives[0].first << "'";
        return archives[0].second;
    }
    else
    {
        std::uniform_int_distribution<size_t> dist(0, archives.size() - 1);
        size_t i = dist(gRandomEngine);
        CLOG(DEBUG, "History") << "Catching up via readable history archive #"
                               << i << ", '" << archives[i].first << "'";
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
    assert(mState == CATCHUP_RETRYING || mState == CATCHUP_VERIFYING);
    mRetryCount = 0;
    mState = CATCHUP_BEGIN;

    assert(mNextLedger > 0);
    uint32_t checkpoint = mNextLedger - 1;

    CLOG(INFO, "History") << "Catchup BEGIN, initLedger=" << mInitLedger
                          << ", guessed nextLedger=" << mNextLedger
                          << ", anchor checkpoint=" << checkpoint;

    uint64_t sleepSeconds =
        mApp.getHistoryManager().nextCheckpointCatchupProbe(mInitLedger);

    mArchive = selectRandomReadableHistoryArchive();
    std::weak_ptr<CatchupStateMachine> weak(shared_from_this());
    mArchive->getSnapState(mApp, checkpoint, [weak, checkpoint, sleepSeconds](
                                                 asio::error_code const& ec,
                                                 HistoryArchiveState const& has)
                           {
                               auto self = weak.lock();
                               if (!self)
                               {
                                   return;
                               }
                               if (ec || checkpoint != has.currentLedger)
                               {
                                   CLOG(WARNING, "History")
                                       << "History archive '"
                                       << self->mArchive->getName()
                                       << "', hasn't yet received checkpoint "
                                       << checkpoint << ", retrying catchup";
                                   self->enterRetryingState(sleepSeconds);
                               }
                               else
                               {
                                   self->enterAnchoredState(has);
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
    auto fi = mFileInfos[name];
    fi->setState(newState);
    if (mState == CATCHUP_ANCHORED || mState == CATCHUP_FETCHING)
    {
        enterFetchingState(fi);
    }
}

// Advance one file, returning true if a new callback has been scheduled.
bool
CatchupStateMachine::advanceFileState(
    std::shared_ptr<FileTransferInfo<FileCatchupState>> fi)
{
    auto& hm = mApp.getHistoryManager();
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
            b = mApp.getBucketManager().getBucketByHash(hash);
        }
        if (b)
        {
            // If for some reason this bucket exists and is live in the
            // BucketManager, just grab a copy of it.
            CLOG(DEBUG, "History")
                << "Existing bucket found in BucketManager: " << hashname;
            mBuckets[hashname] = b;
            fi->setState(FILE_CATCHUP_VERIFIED);
        }
        else
        {
            fi->setState(FILE_CATCHUP_DOWNLOADING);
            CLOG(DEBUG, "History") << "Downloading " << name;
            std::weak_ptr<CatchupStateMachine> weak(shared_from_this());
            hm.getFile(mArchive, fi->remoteName(), fi->localPath_gz(),
                       [weak, name](asio::error_code const& ec)
                       {
                           auto self = weak.lock();
                           if (!self)
                           {
                               return;
                           }
                           self->fileStateChange(ec, name,
                                                 FILE_CATCHUP_DOWNLOADED);
                       });
            return true;
        }
    }
    break;

    case FILE_CATCHUP_DOWNLOADING:
        return true;

    case FILE_CATCHUP_DOWNLOADED:
    {
        fi->setState(FILE_CATCHUP_DECOMPRESSING);
        CLOG(DEBUG, "History") << "Decompressing " << fi->localPath_gz();
        std::weak_ptr<CatchupStateMachine> weak(shared_from_this());
        hm.decompress(
            fi->localPath_gz(), [weak, name](asio::error_code const& ec)
            {
                auto self = weak.lock();
                if (!self)
                {
                    return;
                }
                self->fileStateChange(ec, name, FILE_CATCHUP_DECOMPRESSED);
            });
        return true;
    }

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
            CLOG(DEBUG, "History") << "Not verifying " << name << ", no hash";
            fi->setState(FILE_CATCHUP_VERIFIED);
        }
        else
        {
            CLOG(DEBUG, "History") << "Verifying " << name;
            auto filename = fi->localPath_nogz();
            std::weak_ptr<CatchupStateMachine> weak(shared_from_this());
            hm.verifyHash(
                filename, hash, [weak, name, filename, hashname, hash](
                                    asio::error_code const& ec)
                {
                    auto self = weak.lock();
                    if (!self)
                    {
                        return;
                    }
                    if (!ec)
                    {
                        auto b =
                            self->mApp.getBucketManager().adoptFileAsBucket(
                                filename, hash);
                        self->mBuckets[hashname] = b;
                    }
                    self->fileStateChange(ec, name, FILE_CATCHUP_VERIFIED);
                });
            return true;
        }
        break;

    case FILE_CATCHUP_VERIFYING:
        break;

    case FILE_CATCHUP_VERIFIED:
        break;
    }
    return false;
}

/**
 * Attempt to step the state machine through a file-state-driven state
 * transition. The state machine advances only when all the files have
 * reached the next step in their lifecycle.
 */
void
CatchupStateMachine::enterFetchingState(
    std::shared_ptr<FileTransferInfo<FileCatchupState>> fi)
{
    assert(mState == CATCHUP_ANCHORED || mState == CATCHUP_FETCHING);
    mState = CATCHUP_FETCHING;

    // Fast-path when we've just made a state-change on a single, specific
    // file and are potentially just bouncing off into another callback.
    if (fi && advanceFileState(fi))
    {
        return;
    }

    FileCatchupState minimumState = FILE_CATCHUP_VERIFIED;

    for (auto& pair : mFileInfos)
    {
        auto fi2 = pair.second;
        advanceFileState(fi2);
        minimumState = std::min(fi2->getState(), minimumState);
    }

    if (minimumState == FILE_CATCHUP_FAILED)
    {
        CLOG(WARNING, "History") << "Some fetches failed, retrying";
        enterRetryingState();
    }
    else if (minimumState == FILE_CATCHUP_VERIFIED)
    {
        CLOG(DEBUG, "History") << "All files verified, verifying combination";
        enterVerifyingState();
    }
    else
    {
        CLOG(DEBUG, "History") << "Some fetches still in progress";
        // Do nothing here; in-progress states have callbacks set up already
        // which will fire when they complete.
    }
}

std::shared_ptr<FileCatchupInfo>
CatchupStateMachine::queueTransactionsFile(uint32_t snap)
{
    auto fi =
        std::make_shared<FileCatchupInfo>(FILE_CATCHUP_NEEDED, mDownloadDir,
                                          HISTORY_FILE_TYPE_TRANSACTIONS, snap);
    if (mTransactionInfos.find(snap) == mTransactionInfos.end())
    {
        mTransactionInfos[snap] = fi;
    }
    return fi;
}

std::shared_ptr<FileCatchupInfo>
CatchupStateMachine::queueLedgerFile(uint32_t snap)
{
    auto fi = std::make_shared<FileCatchupInfo>(
        FILE_CATCHUP_NEEDED, mDownloadDir, HISTORY_FILE_TYPE_LEDGER, snap);
    if (mHeaderInfos.find(snap) == mHeaderInfos.end())
    {
        mHeaderInfos[snap] = fi;
    }
    return fi;
}

void
CatchupStateMachine::enterAnchoredState(HistoryArchiveState const& has)
{
    assert(mState == CATCHUP_BEGIN || mState == CATCHUP_RETRYING);
    mState = CATCHUP_ANCHORED;
    mArchiveState = has;

    CLOG(DEBUG, "History") << "Catchup ANCHORED, anchor ledger = "
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
            CLOG(DEBUG, "History") << "Retrying fetch for " << fi->remoteName()
                                   << " from archive '" << mArchive->getName()
                                   << "'";
            fi->setState(FILE_CATCHUP_NEEDED);
        }
    }

    std::vector<std::shared_ptr<FileCatchupInfo>> fileCatchupInfos;
    auto& hm = mApp.getHistoryManager();
    uint32_t freq = hm.getCheckpointFrequency();
    std::vector<std::string> bucketsToFetch;

    // Then make sure all the files we _want_ are either present
    // or queued to be requested.
    if (mMode == HistoryManager::CATCHUP_BUCKET_REPAIR)
    {
        bucketsToFetch =
            mApp.getBucketManager().checkForMissingBucketsFiles(mLocalState);
        auto publishBuckets = mApp.getHistoryManager()
                                  .getMissingBucketsReferencedByPublishQueue();
        bucketsToFetch.insert(bucketsToFetch.end(), publishBuckets.begin(),
                              publishBuckets.end());
    }
    else if (mMode == HistoryManager::CATCHUP_MINIMAL)
    {
        // in CATCHUP_MINIMAL mode we need all the buckets...
        bucketsToFetch = mArchiveState.differingBuckets(mLocalState);

        // ...and _the last_ history ledger file (to get its final state).
        fileCatchupInfos.push_back(
            queueLedgerFile(mArchiveState.currentLedger));
    }
    else
    {
        assert(mMode == HistoryManager::CATCHUP_COMPLETE);
        // In CATCHUP_COMPLETE mode we need all the transaction and ledger
        // files.
        for (uint32_t snap = mArchiveState.currentLedger;
             snap >= mLocalState.currentLedger; snap -= freq)
        {
            fileCatchupInfos.push_back(queueTransactionsFile(snap));
            fileCatchupInfos.push_back(queueLedgerFile(snap));
            if (snap < freq)
            {
                break;
            }
        }
    }

    // Turn bucket hashes into `FileCatchupInfo`s
    for (auto const& h : bucketsToFetch)
    {
        fileCatchupInfos.push_back(std::make_shared<FileCatchupInfo>(
            FILE_CATCHUP_NEEDED, mDownloadDir, HISTORY_FILE_TYPE_BUCKET, h));
    }

    for (auto const& fi : fileCatchupInfos)
    {
        auto name = fi->baseName_nogz();
        if (mFileInfos.find(name) == mFileInfos.end())
        {
            CLOG(DEBUG, "History") << "Starting fetch for " << name
                                   << " from archive '" << mArchive->getName()
                                   << "'";
            mFileInfos[name] = fi;
        }
    }

    enterFetchingState();
}

void
CatchupStateMachine::enterRetryingState(uint64_t nseconds)
{
    assert(mState == CATCHUP_BEGIN || mState == CATCHUP_ANCHORED ||
           mState == CATCHUP_FETCHING || mState == CATCHUP_APPLYING ||
           mState == CATCHUP_VERIFYING);
    bool anchored = mState >= CATCHUP_ANCHORED;
    bool verifying = mState >= CATCHUP_VERIFYING;
    mState = CATCHUP_RETRYING;

    CLOG(WARNING, "History")
        << "Catchup pausing for " << nseconds << " seconds, until "
        << VirtualClock::pointToISOString(mApp.getClock().now() +
                                          std::chrono::seconds(nseconds));

    std::weak_ptr<CatchupStateMachine> weak(shared_from_this());
    mRetryTimer.expires_from_now(std::chrono::seconds(nseconds));
    mRetryTimer.async_wait(
        [weak, anchored, verifying](asio::error_code const& ec)
        {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }
            if (ec)
            {
                CLOG(WARNING, "History")
                    << "Retry timer canceled while waiting";
                return;
            }
            if (self->mRetryCount++ > kRetryLimit)
            {
                CLOG(WARNING, "History") << "Retry count " << kRetryLimit
                                         << " exceeded, restarting catchup";
                self->enterBeginState();
            }
            else if (!anchored)
            {
                CLOG(WARNING, "History")
                    << "Restarting catchup after failure to anchor";
                self->enterBeginState();
            }
            else if (!verifying)
            {
                CLOG(INFO, "History") << "Retrying catchup with archive '"
                                      << self->mArchive->getName() << "'";
                self->enterAnchoredState(self->mArchiveState);
            }
            else
            {
                CLOG(INFO, "History") << "Retrying verify of catchup candidate "
                                      << self->mNextLedger;
                self->enterVerifyingState();
            }
        });
}

void
CatchupStateMachine::enterVerifyingState()
{
    assert(mState == CATCHUP_RETRYING || mState == CATCHUP_FETCHING);
    mState = CATCHUP_VERIFYING;

    if (mMode == HistoryManager::CATCHUP_COMPLETE)
    {
        auto prev = std::make_shared<LedgerHeaderHistoryEntry>(
            mApp.getLedgerManager().getLastClosedLedgerHeader());

        CLOG(INFO, "History") << "Verifying ledger-history chain of "
                              << mHeaderInfos.size()
                              << " transaction-history files from LCL "
                              << LedgerManager::ledgerAbbrev(*prev);

        auto i = mHeaderInfos.begin();
        if (i == mHeaderInfos.end())
        {
            finishVerifyingState(HistoryManager::VERIFY_HASH_OK);
        }
        else
        {
            advanceVerifyingState(prev, i->first);
        }
    }
    else if (mMode == HistoryManager::CATCHUP_MINIMAL)
    {
        // In CATCHUP_MINIMAL mode we just need to acquire the LCL before
        // mNextLedger
        // and check to see if it's acceptable.
        acquireFinalLedgerState(mNextLedger);
        auto status =
            mApp.getLedgerManager().verifyCatchupCandidate(mLastClosed);
        finishVerifyingState(status);
    }
    else
    {
        assert(mMode == HistoryManager::CATCHUP_BUCKET_REPAIR);
        // In CATCHUP_BUCKET_REPAIR, the verification done by the download step
        // is all
        // we can do.
        finishVerifyingState(HistoryManager::VERIFY_HASH_OK);
    }
}

// CATCHUP_VERIFYING has to be split in 3 pieces (enter/advance/finish) so
// that it can bounce off the io_service scheduler in fine enough
// increments to keep normal network traffic flowing, SCP messages flooding
// and such. The same is true for CATCHUP_APPLYING.

void
CatchupStateMachine::advanceVerifyingState(
    std::shared_ptr<LedgerHeaderHistoryEntry> prev, uint32_t checkpoint)
{
    assert(mState == CATCHUP_VERIFYING);
    assert(mMode == HistoryManager::CATCHUP_COMPLETE);

    auto status = HistoryManager::VERIFY_HASH_OK;

    auto i = mHeaderInfos.find(checkpoint);
    if (i != mHeaderInfos.end())
    {
        status = verifyHistoryOfSingleCheckpoint(prev, checkpoint);
        ++i;
    }

    if (status != HistoryManager::VERIFY_HASH_OK || i == mHeaderInfos.end())
    {

        if (prev->header.ledgerSeq + 1 != mNextLedger)
        {
            CLOG(ERROR, "History")
                << "Insufficient history to connect chain to ledger "
                << mNextLedger;
            CLOG(ERROR, "History") << "History chain ends at "
                                   << prev->header.ledgerSeq;
            status = HistoryManager::VERIFY_HASH_BAD;
        }
        status = mApp.getLedgerManager().verifyCatchupCandidate(*prev);
        finishVerifyingState(status);
    }
    else
    {
        uint32_t nextCheckpoint = i->first;
        std::weak_ptr<CatchupStateMachine> weak(shared_from_this());
        mApp.getClock().getIOService().post(
            [weak, prev, nextCheckpoint]()
            {
                auto self = weak.lock();
                if (!self)
                {
                    return;
                }
                self->advanceVerifyingState(prev, nextCheckpoint);
            });
    }
}

void
CatchupStateMachine::finishVerifyingState(
    HistoryManager::VerifyHashStatus status)
{
    assert(mState == CATCHUP_VERIFYING);
    switch (status)
    {
    case HistoryManager::VERIFY_HASH_OK:
        CLOG(DEBUG, "History") << "Catchup material verified, applying";
        enterApplyingState();
        break;
    case HistoryManager::VERIFY_HASH_BAD:
        CLOG(ERROR, "History")
            << "Catchup material failed verification, propagating failure";
        mError = std::make_error_code(std::errc::bad_message);
        enterEndState();
        break;
    case HistoryManager::VERIFY_HASH_UNKNOWN:
        CLOG(WARNING, "History")
            << "Catchup material verification inconclusive, pausing";
        enterRetryingState();
        break;
    default:
        assert(false);
    }
}

static HistoryManager::VerifyHashStatus
verifyLedgerHistoryEntry(LedgerHeaderHistoryEntry const& hhe)
{
    LedgerHeaderFrame lFrame(hhe.header);
    Hash calculated = lFrame.getHash();
    if (calculated != hhe.hash)
    {
        CLOG(ERROR, "History")
            << "Bad ledger-header history entry: claimed ledger "
            << LedgerManager::ledgerAbbrev(hhe) << " actually hashes to "
            << hexAbbrev(calculated);
        return HistoryManager::VERIFY_HASH_BAD;
    }
    return HistoryManager::VERIFY_HASH_OK;
}

static HistoryManager::VerifyHashStatus
verifyLedgerHistoryLink(Hash const& prev, LedgerHeaderHistoryEntry const& curr)
{
    if (verifyLedgerHistoryEntry(curr) != HistoryManager::VERIFY_HASH_OK)
    {
        return HistoryManager::VERIFY_HASH_BAD;
    }
    if (prev != curr.header.previousLedgerHash)
    {
        CLOG(ERROR, "History")
            << "Bad hash-chain: " << LedgerManager::ledgerAbbrev(curr)
            << " wants prev hash " << hexAbbrev(curr.header.previousLedgerHash)
            << " but actual prev hash is " << hexAbbrev(prev);
        return HistoryManager::VERIFY_HASH_BAD;
    }
    return HistoryManager::VERIFY_HASH_OK;
}

HistoryManager::VerifyHashStatus
CatchupStateMachine::verifyHistoryOfSingleCheckpoint(
    std::shared_ptr<LedgerHeaderHistoryEntry> prev, uint32_t checkpoint)
{
    auto i = mHeaderInfos.find(checkpoint);
    assert(i != mHeaderInfos.end());
    auto hi = i->second;

    XDRInputFileStream hdrIn;
    CLOG(DEBUG, "History") << "Verifying ledger headers from "
                           << hi->localPath_nogz() << " starting from ledger "
                           << LedgerManager::ledgerAbbrev(*prev);
    hdrIn.open(hi->localPath_nogz());
    LedgerHeaderHistoryEntry curr;
    while (hdrIn && hdrIn.readOne(curr))
    {
        uint32_t expectedSeq = prev->header.ledgerSeq + 1;
        if (curr.header.ledgerSeq < expectedSeq)
        {
            // Harmless prehistory
            continue;
        }
        else if (curr.header.ledgerSeq > expectedSeq)
        {
            CLOG(ERROR, "History")
                << "History chain overshot expected ledger seq " << expectedSeq
                << ", got " << curr.header.ledgerSeq << " instead";
            return HistoryManager::VERIFY_HASH_BAD;
        }
        if (verifyLedgerHistoryLink(prev->hash, curr) !=
            HistoryManager::VERIFY_HASH_OK)
        {
            return HistoryManager::VERIFY_HASH_BAD;
        }
        *prev = curr;
    }
    return HistoryManager::VERIFY_HASH_OK;
}

void
CatchupStateMachine::enterApplyingState()
{
    assert(mState == CATCHUP_VERIFYING);
    mState = CATCHUP_APPLYING;
    try
    {
        mApplyState = std::make_shared<ApplyState>(mApp);
        if (mMode == HistoryManager::CATCHUP_COMPLETE)
        {
            auto& lm = mApp.getLedgerManager();
            CLOG(DEBUG, "History")
                << "Replaying contents of " << mHeaderInfos.size()
                << " transaction-history files from LCL "
                << LedgerManager::ledgerAbbrev(lm.getLastClosedLedgerHeader());
            mApplyState->mCheckpointNumber =
                (mHeaderInfos.empty() ? 0 : mHeaderInfos.begin()->first);
        }
        else if (mMode == HistoryManager::CATCHUP_MINIMAL)
        {
            CLOG(DEBUG, "History")
                << "Archive bucketListHash: "
                << hexAbbrev(mArchiveState.getBucketListHash());
            CLOG(DEBUG, "History")
                << "mLastClosed bucketListHash: "
                << hexAbbrev(mLastClosed.header.bucketListHash);
        }
        advanceApplyingState();
    }
    catch (std::runtime_error& e)
    {
        CLOG(ERROR, "History") << "Error during apply: " << e.what();
        mError = std::make_error_code(std::errc::bad_message);
        enterEndState();
    }
}

void
CatchupStateMachine::advanceApplyingState()
{
    assert(mState == CATCHUP_APPLYING);
    assert(mApplyState);
    bool keepGoing = true;
    logAndUpdateStatus(true);
    try
    {
        if (mMode == HistoryManager::CATCHUP_MINIMAL)
        {
            // In CATCHUP_MINIMAL mode we're applying the _state_ at mLastClosed
            // without any history replay.
            keepGoing = (mApplyState->mBucketLevel != 0);
            applySingleBucketLevel(mApplyState->mApplyingBuckets,
                                   mApplyState->mBucketLevel);
        }
        else if (mMode == HistoryManager::CATCHUP_COMPLETE)
        {
            // In CATCHUP_COMPLETE mode we're applying the _log_ of history from
            // HistoryManager's LCL through mNextLedger, without any
            // reconstitution.
            auto i = mHeaderInfos.find(mApplyState->mCheckpointNumber);
            if (i != mHeaderInfos.end())
            {
                applyHistoryOfSingleCheckpoint(mApplyState->mCheckpointNumber);
                ++i;
            }
            if (i == mHeaderInfos.end())
            {
                keepGoing = false;
            }
            else
            {
                mApplyState->mCheckpointNumber = i->first;
            }
        }
        else
        {
            assert(mMode == HistoryManager::CATCHUP_BUCKET_REPAIR);
            keepGoing = false;
        }
    }
    catch (std::runtime_error& e)
    {
        CLOG(ERROR, "History") << "Error during apply: " << e.what();
        mError = std::make_error_code(std::errc::bad_message);
    }

    if (keepGoing)
    {
        std::weak_ptr<CatchupStateMachine> weak(shared_from_this());
        mApp.getClock().getIOService().post([weak]()
                                            {
                                                auto self = weak.lock();
                                                if (!self)
                                                {
                                                    return;
                                                }
                                                self->advanceApplyingState();
                                            });
    }
    else
    {
        enterEndState();
    }
}

std::shared_ptr<Bucket>
CatchupStateMachine::getBucketToApply(std::string const& hash)
{
    // Any apply-able bucket is _either_ the empty bucket, or one we downloaded,
    // or one we tried to download but found we already had in the
    // BucketManager, or one we didn't even bother trying to download because
    // mArchiveState.differingBuckets(mLocalState) didn't mention it (in which
    // case it ought to still be in the BucketManager).

    std::shared_ptr<Bucket> b;
    CLOG(DEBUG, "History") << "Searching for bucket to apply: " << hash;
    if (hash.find_first_not_of('0') == std::string::npos)
    {
        b = std::make_shared<Bucket>();
    }
    else
    {
        auto i = mBuckets.find(hash);
        if (i != mBuckets.end())
        {
            b = i->second;
        }
        else
        {
            b = mApp.getBucketManager().getBucketByHash(hexToBin256(hash));
        }
    }
    assert(b);
    return b;
}

void
CatchupStateMachine::applySingleBucketLevel(bool& applying, size_t& n)
{
    auto& db = mApp.getDatabase();
    auto& bl = mApp.getBucketManager().getBucketList();

    CLOG(DEBUG, "History") << "Applying buckets for level " << n
                           << " at ledger " << mLastClosed.header.ledgerSeq;

    // We've verified mLastClosed (in the "trusted part of history" sense) in
    // CATCHUP_VERIFY phase; we now need to check that the BucketListHash we're
    // about to apply is the one denoted by that ledger header.
    if (mLastClosed.header.bucketListHash != mArchiveState.getBucketListHash())
    {
        throw std::runtime_error("catchup BucketList hash differs from "
                                 "BucketList hash in catchup ledger");
    }

    assert(mArchiveState.currentLedger == mLastClosed.header.ledgerSeq);

    // Apply buckets in reverse order, oldest bucket to new. Once we apply
    // one bucket, apply all buckets newer as well.
    HistoryStateBucket& i = mArchiveState.currentBuckets.at(n);
    BucketLevel& existingLevel = bl.getLevel(n);
    --n;

    if (applying || i.snap != binToHex(existingLevel.getSnap()->getHash()))
    {
        std::shared_ptr<Bucket> b = getBucketToApply(i.snap);
        CLOG(DEBUG, "History") << "Applying bucket " << b->getFilename()
                               << " to ledger as BucketList 'snap' for level "
                               << n;
        b->apply(db);
        existingLevel.setSnap(b);
        applying = true;
    }

    if (applying || i.curr != binToHex(existingLevel.getCurr()->getHash()))
    {
        std::shared_ptr<Bucket> b = getBucketToApply(i.curr);
        CLOG(DEBUG, "History") << "Applying bucket " << b->getFilename()
                               << " to ledger as BucketList 'curr' for level "
                               << n;
        b->apply(db);
        existingLevel.setCurr(b);
        applying = true;
    }

    existingLevel.setNext(i.next);

    // Start the merges we need to have completed to resume running at LCL
    bl.restartMerges(mApp, mLastClosed.header.ledgerSeq);
}

void
CatchupStateMachine::acquireFinalLedgerState(uint32_t ledgerNum)
{
    CLOG(DEBUG, "History") << "Seeking ledger state preceding " << ledgerNum;
    assert(mHeaderInfos.size() == 1);
    auto hi = mHeaderInfos.begin()->second;
    XDRInputFileStream hdrIn;
    CLOG(DEBUG, "History") << "Scanning to last-ledger in "
                           << hi->localPath_nogz();
    hdrIn.open(hi->localPath_nogz());
    LedgerHeaderHistoryEntry hHeader;

    // Scan to end.
    bool readOne = false;
    while (hdrIn && hdrIn.readOne(hHeader))
    {
        readOne = true;
    }

    if (!readOne)
    {
        throw std::runtime_error("no ledgers in last-ledger file");
    }

    CLOG(DEBUG, "History") << "Catchup last-ledger header: "
                           << LedgerManager::ledgerAbbrev(hHeader);
    if (hHeader.header.ledgerSeq + 1 != ledgerNum)
    {
        throw std::runtime_error("catchup last-ledger state mismatch");
    }

    mLastClosed = hHeader;
}

void
CatchupStateMachine::applyHistoryOfSingleCheckpoint(uint32_t checkpoint)
{
    auto i = mHeaderInfos.find(checkpoint);
    assert(i != mHeaderInfos.end());

    auto hi = i->second;
    assert(mTransactionInfos.find(checkpoint) != mTransactionInfos.end());
    auto ti = mTransactionInfos[checkpoint];

    XDRInputFileStream hdrIn;
    XDRInputFileStream txIn;

    CLOG(DEBUG, "History") << "Replaying ledger headers from "
                           << hi->localPath_nogz();
    CLOG(DEBUG, "History") << "Replaying transactions from "
                           << ti->localPath_nogz();

    hdrIn.open(hi->localPath_nogz());
    txIn.open(ti->localPath_nogz());

    LedgerHeaderHistoryEntry hHeader;
    LedgerHeader& header = hHeader.header;
    TransactionHistoryEntry txHistoryEntry;
    bool readTxSet = false;

    // Start the merges we need to have completed to play transactions
    // forward from LCL
    auto& lm = mApp.getLedgerManager();
    mApp.getBucketManager().getBucketList().restartMerges(
        mApp, lm.getLastClosedLedgerNum());

    while (hdrIn && hdrIn.readOne(hHeader))
    {
        LedgerHeader const& previousHeader =
            lm.getLastClosedLedgerHeader().header;
        // If we are >1 before LCL, skip
        if (header.ledgerSeq + 1 < previousHeader.ledgerSeq)
        {
            CLOG(DEBUG, "History") << "Catchup skipping old ledger "
                                   << header.ledgerSeq;
            continue;
        }

        // If we are one before LCL, check that we knit up with it
        if (header.ledgerSeq + 1 == previousHeader.ledgerSeq)
        {
            if (hHeader.hash != previousHeader.previousLedgerHash)
            {
                throw std::runtime_error(
                    "replay failed to connect on hash of LCL predecessor");
            }
            CLOG(DEBUG, "History") << "Catchup at 1-before LCL ("
                                   << header.ledgerSeq << "), hash correct";
            continue;
        }

        // If we are at LCL, check that we knit up with it
        if (header.ledgerSeq == previousHeader.ledgerSeq)
        {
            if (hHeader.hash != lm.getLastClosedLedgerHeader().hash)
            {
                throw std::runtime_error("replay at LCL disagreed on hash");
            }
            CLOG(DEBUG, "History") << "Catchup at LCL=" << header.ledgerSeq
                                   << ", hash correct";
            continue;
        }

        // If we are past current, we can't catch up: fail.
        if (header.ledgerSeq != lm.getCurrentLedgerHeader().ledgerSeq)
        {
            throw std::runtime_error("replay overshot current ledger");
        }

        // If we do not agree about LCL hash, we can't catch up: fail.
        if (header.previousLedgerHash != lm.getLastClosedLedgerHeader().hash)
        {
            throw std::runtime_error(
                "replay at current ledger disagreed on LCL hash");
        }
        TxSetFramePtr txset =
            std::make_shared<TxSetFrame>(lm.getLastClosedLedgerHeader().hash);
        if (!readTxSet)
        {
            readTxSet = txIn.readOne(txHistoryEntry);
        }

        CLOG(DEBUG, "History") << "Replaying ledger " << header.ledgerSeq;
        while (readTxSet && txHistoryEntry.ledgerSeq < header.ledgerSeq)
        {
            CLOG(DEBUG, "History") << "Skipping tx for ledger "
                                   << txHistoryEntry.ledgerSeq;
            readTxSet = txIn.readOne(txHistoryEntry);
        }
        if (readTxSet && txHistoryEntry.ledgerSeq == header.ledgerSeq)
        {
            CLOG(DEBUG, "History") << "Preparing tx for ledger "
                                   << txHistoryEntry.ledgerSeq;
            txset = std::make_shared<TxSetFrame>(mApp.getNetworkID(),
                                                 txHistoryEntry.txSet);
            readTxSet = txIn.readOne(txHistoryEntry);
        }
        CLOG(DEBUG, "History") << "Ledger " << header.ledgerSeq << " has "
                               << txset->size() << " transactions";

        // We've verified the ledgerHeader (in the "trusted part of history"
        // sense) in CATCHUP_VERIFY phase; we now need to check that the
        // txhash we're about to apply is the one denoted by that ledger
        // header.
        if (header.scpValue.txSetHash != txset->getContentsHash())
        {
            throw std::runtime_error("replay txset hash differs from txset "
                                     "hash in replay ledger");
        }

        LedgerCloseData closeData(header.ledgerSeq, txset, header.scpValue);
        lm.closeLedger(closeData);

        CLOG(DEBUG, "History")
            << "LedgerManager LCL:\n"
            << xdr::xdr_to_string(lm.getLastClosedLedgerHeader());
        CLOG(DEBUG, "History") << "Replay header:\n"
                               << xdr::xdr_to_string(hHeader);
        if (lm.getLastClosedLedgerHeader().hash != hHeader.hash)
        {
            throw std::runtime_error("replay produced mismatched ledger hash");
        }
        mLastClosed = hHeader;
    }
}

void
CatchupStateMachine::enterEndState()
{
    assert(mState == CATCHUP_APPLYING);
    mApplyState.reset();
    mState = CATCHUP_END;
    CLOG(DEBUG, "History") << "Completed catchup from '" << mArchive->getName()
                           << "', at nextLedger=" << mNextLedger;
    mEndHandler(mError, mMode, mLastClosed);
}
}
