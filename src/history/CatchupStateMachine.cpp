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
#include "util/Logging.h"
#include "util/XDRStream.h"
#include "xdrpp/printer.h"
#include "util/Math.h"

#include <random>
#include <memory>

#define SLEEP_SECONDS_PER_LEDGER (EXP_LEDGER_TIMESPAN_SECONDS+1)

namespace stellar
{

const size_t CatchupStateMachine::kRetryLimit = 16;

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
    , mLocalState(localState)
    , mEndHandler(handler)
    , mState(CATCHUP_RETRYING)
    , mRetryCount(0)
    , mRetryTimer(app)
    , mDownloadDir(app.getTmpDirManager().tmpDir("catchup"))
{
    // We start up in CATCHUP_RETRYING as that's the only valid
    // named pre-state for CATCHUP_BEGIN.
    enterBeginState();
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

    for (auto const& pair : mApp.getConfig().HISTORY)
    {
        if (pair.second->hasGetCmd())
        {
            archives.push_back(pair);
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
    uint32_t blockEnd = mNextLedger - 1;
    uint32_t snap =
        blockEnd / mApp.getHistoryManager().getCheckpointFrequency();

    CLOG(INFO, "History") << "Catchup BEGIN, initLedger=" << mInitLedger
                          << ", guessed nextLedger=" << mNextLedger
                          << ", anchor checkpoint=" << snap;

    uint64_t sleepSeconds =
        mApp.getHistoryManager().nextCheckpointCatchupProbe(mInitLedger);

    mArchive = selectRandomReadableHistoryArchive();
    mArchive->getSnapState(
        mApp, snap, [this, blockEnd, snap, sleepSeconds](
            asio::error_code const& ec,
            HistoryArchiveState const& has)
        {
            if (ec ||
                blockEnd != has.currentLedger)
            {
                CLOG(WARNING, "History")
                    << "History archive '" << this->mArchive->getName()
                    << "', hasn't yet received checkpoint " << snap
                    << ", retrying catchup";
                this->enterRetryingState(sleepSeconds);
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
    if (mState != CATCHUP_RETRYING)
    {
        enterFetchingState();
    }
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
    auto& hm = mApp.getHistoryManager();
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
                CLOG(INFO, "History") << "Downloading " << name;
                hm.getFile(mArchive, fi->remoteName(), fi->localPath_gz(),
                           [this, name](asio::error_code const& ec)
                           {
                               this->fileStateChange(ec, name,
                                                     FILE_CATCHUP_DOWNLOADED);
                           });
            }
        }
        break;

        case FILE_CATCHUP_DOWNLOADING:
            break;

        case FILE_CATCHUP_DOWNLOADED:
            fi->setState(FILE_CATCHUP_DECOMPRESSING);
            CLOG(DEBUG, "History") << "Decompressing " << fi->localPath_gz();
            hm.decompress(
                fi->localPath_gz(), [this, name](asio::error_code const& ec)
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
                CLOG(DEBUG, "History") << "Not verifying " << name
                                      << ", no hash";
                fi->setState(FILE_CATCHUP_VERIFIED);
            }
            else
            {
                CLOG(DEBUG, "History") << "Verifying " << name;
                auto filename = fi->localPath_nogz();
                hm.verifyHash(
                    filename, hash, [this, name, filename, hashname, hash](
                                        asio::error_code const& ec)
                    {
                        if (!ec)
                        {
                            auto b =
                                this->mApp.getBucketManager().adoptFileAsBucket(
                                    filename, hash);
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
            CLOG(INFO, "History") << "Retrying fetch for " << fi->remoteName()
                                  << " from archive '" << mArchive->getName()
                                  << "'";
            fi->setState(FILE_CATCHUP_NEEDED);
        }
    }

    std::vector<std::shared_ptr<FileCatchupInfo>> fileCatchupInfos;
    uint32_t freq = mApp.getHistoryManager().getCheckpointFrequency();
    std::vector<std::string> bucketsToFetch;

    // Then make sure all the files we _want_ are either present
    // or queued to be requested.
    if (mMode == HistoryManager::CATCHUP_BUCKET_REPAIR)
    {
        bucketsToFetch = mApp.getBucketManager().checkForMissingBucketsFiles(mLocalState);
    }
    else if (mMode == HistoryManager::CATCHUP_MINIMAL)
    {
        // in CATCHUP_MINIMAL mode we need all the buckets...
        bucketsToFetch = mArchiveState.differingBuckets(mLocalState);

        // ...and _the last_ history ledger file (to get its final state).
        uint32_t snap = mArchiveState.currentLedger / freq;
        fileCatchupInfos.push_back(queueLedgerFile(snap));
    }
    else
    {
        assert(mMode == HistoryManager::CATCHUP_COMPLETE);
        // In CATCHUP_COMPLETE mode we need all the transaction and ledger
        // files.
        for (uint32_t snap = mLocalState.currentLedger / freq;
             snap <= mArchiveState.currentLedger / freq; ++snap)
        {
            fileCatchupInfos.push_back(queueTransactionsFile(snap));
            fileCatchupInfos.push_back(queueLedgerFile(snap));
        }
    }

    // Turn bucket hashes into `FileCatchupInfo`s
    for (auto const& h : bucketsToFetch)
    {
        fileCatchupInfos.push_back(std::make_shared<FileCatchupInfo>(
            FILE_CATCHUP_NEEDED, mDownloadDir, HISTORY_FILE_TYPE_BUCKET,
            h));
    }


    for (auto const& fi : fileCatchupInfos)
    {
        auto name = fi->baseName_nogz();
        if (mFileInfos.find(name) == mFileInfos.end())
        {
            CLOG(INFO, "History") << "Starting fetch for " << name
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
        << "Catchup pausing for " << nseconds
        << " seconds, until "
        << VirtualClock::pointToISOString(mApp.getClock().now() +
                                          std::chrono::seconds(nseconds));

    mRetryTimer.expires_from_now(std::chrono::seconds(nseconds));
    mRetryTimer.async_wait(
        [this, anchored, verifying](asio::error_code const& ec)
        {
            if (ec)
            {
                CLOG(WARNING, "History")
                    << "Retry timer canceled while waiting";
                return;
            }
            if (this->mRetryCount++ > kRetryLimit)
            {
                CLOG(WARNING, "History") << "Retry count " << kRetryLimit
                                         << " exceeded, restarting catchup";
                this->enterBeginState();
            }
            else if (!anchored)
            {
                CLOG(WARNING, "History")
                    << "Unable to anchor, restarting catchup";
                this->enterBeginState();
            }
            else if (!verifying)
            {
                CLOG(INFO, "History") << "Retrying catchup with archive '"
                                      << this->mArchive->getName() << "'";
                this->enterAnchoredState(this->mArchiveState);
            }
            else
            {
                CLOG(INFO, "History") << "Retrying verify of catchup candidate "
                                      << this->mNextLedger;
                this->enterVerifyingState();
            }
        });
}

void
CatchupStateMachine::enterVerifyingState()
{
    assert(mState == CATCHUP_RETRYING || mState == CATCHUP_FETCHING);

    mState = CATCHUP_VERIFYING;

    HistoryManager::VerifyHashStatus status =
        HistoryManager::VERIFY_HASH_UNKNOWN;
    if (mMode == HistoryManager::CATCHUP_COMPLETE)
    {
        // In CATCHUP_COMPLETE mode we need to verify he whole history chain;
        // this includes checking the final LCL of the chain with LedgerManager.
        status = verifyHistoryFromLastClosedLedger();
    }
    else if (mMode == HistoryManager::CATCHUP_MINIMAL)
    {
        // In CATCHUP_MINIMAL mode we just need to acquire the LCL before
        // mNextLedger
        // and check to see if it's acceptable.
        acquireFinalLedgerState(mNextLedger);
        status = mApp.getLedgerManager().verifyCatchupCandidate(mLastClosed);
    } else
    {
        assert(mMode == HistoryManager::CATCHUP_BUCKET_REPAIR);
        // In CATCH_BUCKET_REPAIR, the verification done by the download step is all 
        // we can do.
        status = HistoryManager::VERIFY_HASH_OK;
    }

    switch (status)
    {
    case HistoryManager::VERIFY_HASH_OK:
        CLOG(DEBUG, "History") << "Catchup material verified, applying";
        enterApplyingState();
        break;
    case HistoryManager::VERIFY_HASH_BAD:
        CLOG(INFO, "History")
            << "Catchup material failed verification, restarting";
        enterBeginState();
        break;
    case HistoryManager::VERIFY_HASH_UNKNOWN:
        CLOG(INFO, "History")
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
CatchupStateMachine::verifyHistoryFromLastClosedLedger()
{
    auto& lm = mApp.getLedgerManager();
    LedgerHeaderHistoryEntry prev = lm.getLastClosedLedgerHeader();
    CLOG(INFO, "History") << "Verifying ledger-history chain of "
                          << mHeaderInfos.size()
                          << " transaction-history files from LCL "
                          << LedgerManager::ledgerAbbrev(prev);

    for (auto& pair : mHeaderInfos)
    {
        auto hi = pair.second;
        XDRInputFileStream hdrIn;
        CLOG(INFO, "History")
            << "Verifying ledger headers from " << hi->localPath_nogz()
            << " starting from ledger " << LedgerManager::ledgerAbbrev(prev);
        hdrIn.open(hi->localPath_nogz());
        LedgerHeaderHistoryEntry curr;
        while (hdrIn && hdrIn.readOne(curr))
        {
            uint32_t expectedSeq = prev.header.ledgerSeq + 1;
            if (curr.header.ledgerSeq < expectedSeq)
            {
                // Harmless prehistory
                continue;
            }
            else if (curr.header.ledgerSeq > expectedSeq)
            {
                CLOG(ERROR, "History")
                    << "History chain overshot expected ledger seq "
                    << expectedSeq;
                return HistoryManager::VERIFY_HASH_BAD;
            }
            if (verifyLedgerHistoryLink(prev.hash, curr) !=
                HistoryManager::VERIFY_HASH_OK)
            {
                return HistoryManager::VERIFY_HASH_BAD;
            }
            prev = curr;
        }
    }
    if (prev.header.ledgerSeq + 1 != mNextLedger)
    {
        CLOG(INFO, "History")
            << "Insufficient history to connect chain to ledger" << mNextLedger;
        return HistoryManager::VERIFY_HASH_BAD;
    }
    return lm.verifyCatchupCandidate(prev);
}

void
CatchupStateMachine::enterApplyingState()
{
    assert(mState == CATCHUP_VERIFYING);
    mState = CATCHUP_APPLYING;
    auto& db = mApp.getDatabase();
    auto& sess = db.getSession();
    soci::transaction sqltx(sess);

    // FIXME: this should do a "pre-apply scan" of the incoming contents
    // to confirm that it's part of the trusted chain of history we want
    // to catch up with. Currently it applies blindly.

    if (mMode == HistoryManager::CATCHUP_MINIMAL)
    {
        // In CATCHUP_MINIMAL mode we're applying the _state_ at mLastClosed
        // without any history replay.
        applyBucketsAtLastClosedLedger();
    }
    else if (mMode == HistoryManager::CATCHUP_COMPLETE)
    {
        // In CATCHUP_COMPLETE mode we're applying the _log_ of history from
        // HistoryManager's LCL through mNextLedger, without any reconstitution.
        applyHistoryFromLastClosedLedger();
    } else
    {
        assert(mMode == HistoryManager::CATCHUP_BUCKET_REPAIR);
        // Nothing to do.
    }

    sqltx.commit();
    enterEndState();
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
CatchupStateMachine::applyBucketsAtLastClosedLedger()
{
    auto& db = mApp.getDatabase();
    auto& bl = mApp.getBucketManager().getBucketList();
    auto n = BucketList::kNumLevels;
    bool applying = false;

    CLOG(INFO, "History") << "Applying buckets at ledger "
                          << mLastClosed.header.ledgerSeq;

    // We've verified mLastClosed (in the "trusted part of history" sense) in
    // CATCHUP_VERIFY phase; we now need to check that the BucketListHash we're
    // about to apply is the one denoted by that ledger header.
    if (mLastClosed.header.bucketListHash != mArchiveState.getBucketListHash())
    {
        throw std::runtime_error("catchup BucketList hash differs from "
                                 "BucketList hash in catchup ledger");
    }

    assert(mArchiveState.currentLedger == mLastClosed.header.ledgerSeq);

    CLOG(INFO, "History") << "Archive bucketListHash: "
                          << hexAbbrev(mArchiveState.getBucketListHash());
    CLOG(INFO, "History") << "mLastClosed bucketListHash: "
                          << hexAbbrev(mLastClosed.header.bucketListHash);

    // Apply buckets in reverse order, oldest bucket to new. Once we apply
    // one bucket, apply all buckets newer as well.
    for (auto i = mArchiveState.currentBuckets.rbegin();
         i != mArchiveState.currentBuckets.rend(); i++)
    {
        --n;
        BucketLevel& existingLevel = bl.getLevel(n);

        if (applying || i->snap != binToHex(existingLevel.getSnap()->getHash()))
        {
            std::shared_ptr<Bucket> b = getBucketToApply(i->snap);
            CLOG(DEBUG, "History")
                << "Applying bucket " << b->getFilename()
                << " to ledger as BucketList 'snap' for level " << n;
            b->apply(db);
            existingLevel.setSnap(b);
            applying = true;
        }

        if (applying || i->curr != binToHex(existingLevel.getCurr()->getHash()))
        {
            std::shared_ptr<Bucket> b = getBucketToApply(i->curr);
            CLOG(DEBUG, "History")
                << "Applying bucket " << b->getFilename()
                << " to ledger as BucketList 'curr' for level " << n;
            b->apply(db);
            existingLevel.setCurr(b);
            applying = true;
        }
    }

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
CatchupStateMachine::applyHistoryFromLastClosedLedger()
{
    auto& lm = mApp.getLedgerManager();
    CLOG(INFO, "History") << "Replaying contents of " << mHeaderInfos.size()
                          << " transaction-history files from LCL "
                          << LedgerManager::ledgerAbbrev(
                                 lm.getLastClosedLedgerHeader());

    for (auto pair : mHeaderInfos)
    {
        auto checkpoint = pair.first;
        auto hi = pair.second;
        assert(mTransactionInfos.find(checkpoint) != mTransactionInfos.end());
        auto ti = mTransactionInfos[checkpoint];

        XDRInputFileStream hdrIn;
        XDRInputFileStream txIn;

        CLOG(INFO, "History") << "Replaying ledger headers from "
                              << hi->localPath_nogz();
        CLOG(INFO, "History") << "Replaying transactions from "
                              << ti->localPath_nogz();

        hdrIn.open(hi->localPath_nogz());
        txIn.open(ti->localPath_nogz());

        LedgerHeaderHistoryEntry hHeader;
        LedgerHeader& header = hHeader.header;
        TransactionHistoryEntry txHistoryEntry;
        bool readTxSet = false;

        // Start the merges we need to have completed to play transactions
        // forward from LCL
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
            if (header.previousLedgerHash !=
                lm.getLastClosedLedgerHeader().hash)
            {
                throw std::runtime_error(
                    "replay at current ledger disagreed on LCL hash");
            }
            TxSetFramePtr txset = std::make_shared<TxSetFrame>(
                lm.getLastClosedLedgerHeader().hash);
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
                txset = std::make_shared<TxSetFrame>(txHistoryEntry.txSet);
                readTxSet = txIn.readOne(txHistoryEntry);
            }
            CLOG(DEBUG, "History") << "Ledger " << header.ledgerSeq << " has "
                                   << txset->size() << " transactions";

            // We've verified the ledgerHeader (in the "trusted part of history"
            // sense) in CATCHUP_VERIFY phase; we now need to check that the
            // txhash we're about to apply is the one denoted by that ledger
            // header.
            if (header.txSetHash != txset->getContentsHash())
            {
                throw std::runtime_error("replay txset hash differs from txset "
                                         "hash in replay ledger");
            }

            LedgerCloseData closeData(header.ledgerSeq, txset, header.closeTime,
                                      header.baseFee);
            lm.closeLedger(closeData);

            CLOG(DEBUG, "History")
                << "LedgerManager LCL:\n"
                << xdr::xdr_to_string(lm.getLastClosedLedgerHeader());
            CLOG(DEBUG, "History") << "Replay header:\n"
                                   << xdr::xdr_to_string(hHeader);
            if (lm.getLastClosedLedgerHeader().hash != hHeader.hash)
            {
                throw std::runtime_error(
                    "replay produced mismatched ledger hash");
            }
            mLastClosed = hHeader;
        }
    }
}

void
CatchupStateMachine::enterEndState()
{
    assert(mState == CATCHUP_APPLYING);
    mState = CATCHUP_END;
    CLOG(DEBUG, "History") << "Completed catchup from '" << mArchive->getName()
                           << "', at nextLedger=" << mNextLedger;
    mEndHandler(mError, mMode, mLastClosed);
}
}
