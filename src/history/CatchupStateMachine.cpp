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
#include "database/Database.h"
#include "herder/TxSetFrame.h"
#include "ledger/LedgerDelta.h"
#include "ledger/LedgerGateway.h"
#include "transactions/TransactionFrame.h"
#include "util/Logging.h"
#include "util/XDRStream.h"
#include "xdrpp/printer.h"

#include <random>
#include <memory>

namespace stellar
{

const size_t
CatchupStateMachine::kRetryLimit = 16;

CatchupStateMachine::CatchupStateMachine(
    Application& app,
    uint32_t lastLedger,
    uint32_t initLedger,
    HistoryMaster::ResumeMode mode,
    std::function<void(asio::error_code const& ec,
                       HistoryMaster::ResumeMode mode,
                       LedgerHeaderHistoryEntry const& lastClosed)> handler)
    : mApp(app)
    , mLastLedger(lastLedger)
    , mInitLedger(initLedger)
    , mNextLedger(HistoryMaster::nextCheckpointLedger(initLedger))
    , mMode(mode)
    , mEndHandler(handler)
    , mState(CATCHUP_RETRYING)
    , mRetryCount(0)
    , mRetryTimer(app)
    , mDownloadDir(app.getTmpDirMaster().tmpDir("catchup"))
{
    // We start up in CATCHUP_RETRYING as that's the only valid
    // named pre-state for CATCHUP_BEGIN.
    mLocalState = app.getHistoryMaster().getLastClosedHistoryArchiveState();
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

    assert(mNextLedger > 0);
    uint32_t blockEnd = mNextLedger - 1;
    uint32_t snap = blockEnd / HistoryMaster::kCheckpointFrequency;

    CLOG(INFO, "History")
        << "Catchup BEGIN, initLedger=" << mInitLedger
        << ", guessed nextLedger=" << mNextLedger
        << ", anchor checkpoint=" << snap;

    mArchive = selectRandomReadableHistoryArchive();
    mArchive->getSnapState(
        mApp,
        snap,
        [this, blockEnd, snap](asio::error_code const& ec,
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
                if (blockEnd != has.currentLedger)
                {
                    CLOG(WARNING, "History")
                        << "History archive '"
                        << this->mArchive->getName()
                        << "', hasn't yet received checkpoint "
                        << snap
                        << ", retrying catchup";
                    this->enterRetryingState();
                }
                else
                {
                    this->enterAnchoredState(has);
                }
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
                    fi->remoteName(), fi->localPath_gz(),
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
            CLOG(INFO, "History") << "Decompressing " << fi->localPath_gz();
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

std::shared_ptr<FileCatchupInfo>
CatchupStateMachine::queueTransactionsFile(uint32_t snap)
{
    auto fi = std::make_shared<FileCatchupInfo>(
        FILE_CATCHUP_NEEDED, mDownloadDir,
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
        FILE_CATCHUP_NEEDED, mDownloadDir,
        HISTORY_FILE_TYPE_LEDGER, snap);
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
                << "Retrying fetch for " << fi->remoteName()
                << " from archive '" << mArchive->getName() << "'";
            fi->setState(FILE_CATCHUP_NEEDED);
        }
    }

    std::vector<std::shared_ptr<FileCatchupInfo>> fileCatchupInfos;

    // Then make sure all the files we _want_ are either present
    // or queued to be requested.
    if (mMode == HistoryMaster::RESUME_AT_NEXT)
    {
        // in RESUME_AT_NEXT mode we need all the buckets...
        std::vector<std::string> bucketsToFetch = mArchiveState.differingBuckets(mLocalState);
        for (auto const& h : bucketsToFetch)
        {
            fileCatchupInfos.push_back(
                std::make_shared<FileCatchupInfo>(
                    FILE_CATCHUP_NEEDED, mDownloadDir,
                    HISTORY_FILE_TYPE_BUCKET, h));
        }

        // ...and _the last_ history ledger file (to get its final state).
        uint32_t snap = mArchiveState.currentLedger / HistoryMaster::kCheckpointFrequency;
        fileCatchupInfos.push_back(queueLedgerFile(snap));
    }
    else
    {
        assert(mMode == HistoryMaster::RESUME_AT_LAST);
        // In RESUME_AT_LAST mode we need all the transaction and ledger files.
        for (uint32_t snap = mLocalState.currentLedger / HistoryMaster::kCheckpointFrequency;
             snap <= mArchiveState.currentLedger / HistoryMaster::kCheckpointFrequency;
             ++snap)
        {
            fileCatchupInfos.push_back(queueTransactionsFile(snap));
            fileCatchupInfos.push_back(queueLedgerFile(snap));
        }
    }
    for (auto const& fi : fileCatchupInfos)
    {
        auto name = fi->baseName_nogz();
        if (mFileInfos.find(name) == mFileInfos.end())
        {
            CLOG(INFO, "History")
                << "Starting fetch for " << name
                << " from archive '" << mArchive->getName() << "'";
            mFileInfos[name] = fi;
        }
    }

    enterFetchingState();
}

void
CatchupStateMachine::enterRetryingState()
{
    assert(mState == CATCHUP_BEGIN || mState == CATCHUP_ANCHORED ||
           mState == CATCHUP_FETCHING || mState == CATCHUP_APPLYING);
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

void
CatchupStateMachine::enterApplyingState()
{
    assert(mState == CATCHUP_FETCHING);
    mState = CATCHUP_APPLYING;
    auto& db = mApp.getDatabase();
    auto& sess = db.getSession();
    soci::transaction sqltx(sess);

    // FIXME: this should do a "pre-apply scan" of the incoming contents
    // to confirm that it's part of the trusted chain of history we want
    // to catch up with. Currently it applies blindly.

    if (mMode == HistoryMaster::RESUME_AT_NEXT)
    {
        // In RESUME_AT_NEXT mode we're applying the _state_ at mNextLedger
        // without any history replay.
        applyBucketsAtLedger(mNextLedger);
        acquireFinalLedgerState(mNextLedger);
    }
    else
    {
        // In RESUME_AT_LAST mode we're applying the _log_ of history from
        // mLastLedger through mNextLedger, without any reconstitution.
        assert(mMode == HistoryMaster::RESUME_AT_LAST);
        applyHistoryFromLedger(mLastLedger);
    }

    sqltx.commit();
    enterEndState();
}

void
CatchupStateMachine::applyBucketsAtLedger(uint32_t ledgerNum)
{
    auto& db = mApp.getDatabase();
    auto& bl = mApp.getCLFMaster().getBucketList();
    auto n = BucketList::kNumLevels;
    bool applying = false;

    CLOG(INFO, "History") << "Applying buckets at ledger " << ledgerNum;

    // Apply buckets in reverse order, oldest bucket to new. Once we apply
    // one bucket, apply all buckets newer as well.
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
            CLOG(DEBUG, "History") << "Applying bucket " << b->getFilename()
                                   << " to ledger as CLF 'snap' for level " << n;
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
            CLOG(DEBUG, "History") << "Applying bucket " << b->getFilename()
                                   << " to ledger as CLF 'curr' for level " << n;
            b->apply(db);
            existingLevel.setCurr(b);
            applying = true;
        }
    }
}

void
CatchupStateMachine::acquireFinalLedgerState(uint32_t ledgerNum)
{
    CLOG(INFO, "History") << "Seeking ledger state preceding " << ledgerNum;
    assert(mHeaderInfos.size() == 1);
    auto hi = mHeaderInfos.begin()->second;
    XDRInputFileStream hdrIn;
    CLOG(INFO, "History") << "Scanning to last-ledger in " << hi->localPath_nogz();
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

    CLOG(INFO, "History") << "Catchup last-ledger header: " << LedgerMaster::ledgerAbbrev(hHeader);
    if (hHeader.header.ledgerSeq + 1 != ledgerNum)
    {
        throw std::runtime_error("catchup last-ledger state mismatch");
    }

    mLastClosed = hHeader;
}


void
CatchupStateMachine::applyHistoryFromLedger(uint32_t ledgerNum)
{
    CLOG(INFO, "History") << "Replaying contents of " << mHeaderInfos.size()
                          << " transaction-history files from ledger " << ledgerNum;

    auto& lm = mApp.getLedgerMaster();
    for (auto pair : mHeaderInfos)
    {
        auto checkpoint = pair.first;
        auto hi = pair.second;
        assert(mTransactionInfos.find(checkpoint) != mTransactionInfos.end());
        auto ti = mTransactionInfos[checkpoint];

        XDRInputFileStream hdrIn;
        XDRInputFileStream txIn;

        CLOG(INFO, "History") << "Replaying ledger headers from " << hi->localPath_nogz();
        CLOG(INFO, "History") << "Replaying transactions from " << ti->localPath_nogz();

        hdrIn.open(hi->localPath_nogz());
        txIn.open(ti->localPath_nogz());

        LedgerHeaderHistoryEntry hHeader;
        LedgerHeader &header = hHeader.header;
        TransactionHistoryEntry txHistoryEntry;
        bool readTxSet = false;

        while (hdrIn && hdrIn.readOne(hHeader))
        {
            LedgerHeader const& previousHeader = lm.getLastClosedLedgerHeader().header;
            // If we are >1 before LCL, skip
            if (header.ledgerSeq + 1 < previousHeader.ledgerSeq)
            {
                CLOG(DEBUG, "History") << "Catchup skipping old ledger " << header.ledgerSeq;
                continue;
            }

            // If we are one before LCL, check that we knit up with it
            if (header.ledgerSeq + 1 == previousHeader.ledgerSeq)
            {
                if (hHeader.hash != previousHeader.previousLedgerHash)
                {
                    throw std::runtime_error("replay failed to connect on hash of LCL predecessor");
                }
                CLOG(DEBUG, "History") << "Catchup at 1-before LCL (" << header.ledgerSeq << "), hash correct";
                continue;
            }

            // If we are at LCL, check that we knit up with it
            if (header.ledgerSeq == previousHeader.ledgerSeq)
            {
                if (hHeader.hash != lm.getLastClosedLedgerHeader().hash)
                {
                    throw std::runtime_error("replay at LCL disagreed on hash");
                }
                CLOG(DEBUG, "History") << "Catchup at LCL=" << header.ledgerSeq << ", hash correct";
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
                throw std::runtime_error("replay at current ledger disagreed on LCL hash");
            }

            TxSetFramePtr txset = std::make_shared<TxSetFrame>(lm.getLastClosedLedgerHeader().hash);
            if (!readTxSet)
            {
                readTxSet = txIn.readOne(txHistoryEntry);
            }

            CLOG(DEBUG, "History") << "Replaying ledger " << header.ledgerSeq;
            while (readTxSet && txHistoryEntry.ledgerSeq < header.ledgerSeq)
            {
                CLOG(DEBUG, "History") << "Skipping tx for ledger " << txHistoryEntry.ledgerSeq;
                readTxSet = txIn.readOne(txHistoryEntry);
            }
            if (readTxSet && txHistoryEntry.ledgerSeq == header.ledgerSeq)
            {
                CLOG(DEBUG, "History") << "Preparing tx for ledger " << txHistoryEntry.ledgerSeq;
                txset = make_shared<TxSetFrame>(txHistoryEntry.txSet);
                readTxSet = txIn.readOne(txHistoryEntry);
            }
            CLOG(DEBUG, "History") << "Ledger " << header.ledgerSeq
                                   << " has " << txset->size() << " transactions";
            LedgerCloseData closeData(header.ledgerSeq,
                                      txset,
                                      header.closeTime,
                                      header.baseFee);
            lm.closeLedger(closeData);

            CLOG(DEBUG, "History") << "LedgerMaster LCL:\n" << xdr::xdr_to_string(lm.getLastClosedLedgerHeader());
            CLOG(DEBUG, "History") << "Replay header:\n" << xdr::xdr_to_string(hHeader);
            if (lm.getLastClosedLedgerHeader().hash != hHeader.hash)
            {
                throw std::runtime_error("replay produced mismatched ledger hash");
            }
            mLastClosed = hHeader;
        }
    }
}

void CatchupStateMachine::enterEndState()
{
    assert(mState == CATCHUP_APPLYING);
    mState = CATCHUP_END;
    CLOG(DEBUG, "History")
        << "Completed catchup from '" << mArchive->getName() << "', at nextLedger=" << mNextLedger;
    mEndHandler(mError, mMode, mLastClosed);
}


}
