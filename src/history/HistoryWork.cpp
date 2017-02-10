// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "history/HistoryWork.h"
#include "bucket/BucketApplicator.h"
#include "bucket/BucketManager.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "herder/LedgerCloseData.h"
#include "herder/TxSetFrame.h"
#include "history/HistoryManager.h"
#include "history/StateSnapshot.h"
#include "ledger/LedgerHeaderFrame.h"
#include "ledger/LedgerManager.h"
#include "main/Config.h"
#include "process/ProcessManager.h"
#include "util/Logging.h"
#include "util/make_unique.h"
#include "xdr/Stellar-ledger.h"
#include "xdrpp/printer.h"

#include "lib/util/format.h"

#include <fstream>

namespace stellar
{

static std::string
fmtProgress(Application& app, std::string const& task, uint32_t first,
            uint32_t last, uint32_t curr)
{
    auto step = app.getHistoryManager().getCheckpointFrequency();
    if (curr > last)
    {
        curr = last;
    }
    if (curr < first)
    {
        curr = first;
    }
    if (step == 0)
    {
        step = 1;
    }
    if (last < first)
    {
        last = first;
    }
    auto done = 1 + ((curr - first) / step);
    auto total = 1 + ((last - first) / step);
    auto pct = (100 * done) / total;
    return fmt::format("{:s} {:d}/{:d} ({:d}%)", task, done, total, pct);
}

RunCommandWork::RunCommandWork(Application& app, WorkParent& parent,
                               std::string const& uniqueName, size_t maxRetries)
    : Work(app, parent, uniqueName, maxRetries)
{
}

void
RunCommandWork::onStart()
{
    std::string cmd, outfile;
    getCommand(cmd, outfile);
    if (!cmd.empty())
    {
        auto exit = mApp.getProcessManager().runProcess(cmd, outfile);
        exit.async_wait(callComplete());
    }
    else
    {
        scheduleSuccess();
    }
}

void
RunCommandWork::onRun()
{
    // Do nothing: we ran the command in onStart().
}

GetRemoteFileWork::GetRemoteFileWork(
    Application& app, WorkParent& parent, std::string const& remote,
    std::string const& local, std::shared_ptr<HistoryArchive const> archive,
    size_t maxRetries)
    : RunCommandWork(app, parent, std::string("get-remote-file ") + remote,
                     maxRetries)
    , mRemote(remote)
    , mLocal(local)
    , mArchive(archive)
{
}

void
GetRemoteFileWork::getCommand(std::string& cmdLine, std::string& outFile)
{
    auto archive = mArchive;
    if (!archive)
    {
        archive = mApp.getHistoryManager().selectRandomReadableHistoryArchive();
    }
    assert(archive);
    assert(archive->hasGetCmd());
    cmdLine = archive->getFileCmd(mRemote, mLocal);
}

void
GetRemoteFileWork::onReset()
{
    std::remove(mLocal.c_str());
}

PutRemoteFileWork::PutRemoteFileWork(
    Application& app, WorkParent& parent, std::string const& local,
    std::string const& remote, std::shared_ptr<HistoryArchive const> archive)
    : RunCommandWork(app, parent, std::string("put-remote-file ") + remote)
    , mRemote(remote)
    , mLocal(local)
    , mArchive(archive)
{
    assert(mArchive);
    assert(mArchive->hasPutCmd());
}

void
PutRemoteFileWork::getCommand(std::string& cmdLine, std::string& outFile)
{
    cmdLine = mArchive->putFileCmd(mLocal, mRemote);
}

MakeRemoteDirWork::MakeRemoteDirWork(
    Application& app, WorkParent& parent, std::string const& dir,
    std::shared_ptr<HistoryArchive const> archive)
    : RunCommandWork(app, parent, std::string("make-remote-dir ") + dir)
    , mDir(dir)
    , mArchive(archive)
{
    assert(mArchive);
}

void
MakeRemoteDirWork::getCommand(std::string& cmdLine, std::string& outFile)
{
    if (mArchive->hasMkdirCmd())
    {
        cmdLine = mArchive->mkdirCmd(mDir);
    }
}

///////////////////////////////////////////////////////////////////////////
// Gzip and Gunzip
///////////////////////////////////////////////////////////////////////////

static void
checkGzipSuffix(std::string const& filename)
{
    std::string suf(".gz");
    if (!(filename.size() >= suf.size() &&
          equal(suf.rbegin(), suf.rend(), filename.rbegin())))
    {
        throw std::runtime_error("filename does not end in .gz");
    }
}

static void
checkNoGzipSuffix(std::string const& filename)
{
    std::string suf(".gz");
    if (filename.size() >= suf.size() &&
        equal(suf.rbegin(), suf.rend(), filename.rbegin()))
    {
        throw std::runtime_error("filename ends in .gz");
    }
}

GzipFileWork::GzipFileWork(Application& app, WorkParent& parent,
                           std::string const& filenameNoGz, bool keepExisting)
    : RunCommandWork(app, parent, std::string("gzip-file ") + filenameNoGz)
    , mFilenameNoGz(filenameNoGz)
    , mKeepExisting(keepExisting)
{
    checkNoGzipSuffix(mFilenameNoGz);
}

void
GzipFileWork::onReset()
{
    std::string filenameGz = mFilenameNoGz + ".gz";
    std::remove(filenameGz.c_str());
}

void
GzipFileWork::getCommand(std::string& cmdLine, std::string& outFile)
{
    cmdLine = "gzip ";
    if (mKeepExisting)
    {
        cmdLine += "-c ";
        outFile = mFilenameNoGz + ".gz";
    }
    cmdLine += mFilenameNoGz;
}

GunzipFileWork::GunzipFileWork(Application& app, WorkParent& parent,
                               std::string const& filenameGz, bool keepExisting,
                               size_t maxRetries)
    : RunCommandWork(app, parent, std::string("gunzip-file ") + filenameGz,
                     maxRetries)
    , mFilenameGz(filenameGz)
    , mKeepExisting(keepExisting)
{
    checkGzipSuffix(mFilenameGz);
}

void
GunzipFileWork::getCommand(std::string& cmdLine, std::string& outFile)
{
    cmdLine = "gzip -d ";
    if (mKeepExisting)
    {
        cmdLine += "-c ";
        outFile = mFilenameGz.substr(0, mFilenameGz.size() - 3);
    }
    cmdLine += mFilenameGz;
}

void
GunzipFileWork::onReset()
{
    std::string filenameNoGz = mFilenameGz.substr(0, mFilenameGz.size() - 3);
    std::remove(filenameNoGz.c_str());
}

///////////////////////////////////////////////////////////////////////////
// Verify Buckets and Ledger Chains
///////////////////////////////////////////////////////////////////////////

VerifyBucketWork::VerifyBucketWork(
    Application& app, WorkParent& parent,
    std::map<std::string, std::shared_ptr<Bucket>>& buckets,
    std::string const& bucketFile, uint256 const& hash)
    : Work(app, parent, std::string("verify-bucket-hash ") + bucketFile)
    , mBuckets(buckets)
    , mBucketFile(bucketFile)
    , mHash(hash)
{
    checkNoGzipSuffix(mBucketFile);
}

void
VerifyBucketWork::onStart()
{
    std::string filename = mBucketFile;
    uint256 hash = mHash;
    Application& app = this->mApp;
    auto handler = callComplete();
    app.getWorkerIOService().post([&app, filename, handler, hash]() {
        auto hasher = SHA256::create();
        asio::error_code ec;
        char buf[4096];
        {
            // ensure that the stream gets its own scope to avoid race with
            // main thread
            std::ifstream in(filename, std::ifstream::binary);
            while (in)
            {
                in.read(buf, sizeof(buf));
                hasher->add(ByteSlice(buf, in.gcount()));
            }
            uint256 vHash = hasher->finish();
            if (vHash == hash)
            {
                CLOG(DEBUG, "History") << "Verified hash (" << hexAbbrev(hash)
                                       << ") for " << filename;
            }
            else
            {
                CLOG(WARNING, "History") << "FAILED verifying hash for "
                                         << filename;
                CLOG(WARNING, "History") << "expected hash: " << binToHex(hash);
                CLOG(WARNING, "History") << "computed hash: "
                                         << binToHex(vHash);
                ec = std::make_error_code(std::errc::io_error);
            }
        }
        app.getClock().getIOService().post([ec, handler]() { handler(ec); });
    });
}

void
VerifyBucketWork::onRun()
{
    // Do nothing: we spawned the verifier in onStart().
}

Work::State
VerifyBucketWork::onSuccess()
{
    auto b = mApp.getBucketManager().adoptFileAsBucket(mBucketFile, mHash);
    mBuckets[binToHex(mHash)] = b;
    return WORK_SUCCESS;
}

VerifyLedgerChainWork::VerifyLedgerChainWork(
    Application& app, WorkParent& parent, TmpDir const& downloadDir,
    uint32_t first, uint32_t last, bool manualCatchup,
    LedgerHeaderHistoryEntry& firstVerified,
    LedgerHeaderHistoryEntry& lastVerified)
    : Work(app, parent, "verify-ledger-chain")
    , mDownloadDir(downloadDir)
    , mFirstSeq(first)
    , mCurrSeq(first)
    , mLastSeq(last)
    , mManualCatchup(manualCatchup)
    , mFirstVerified(firstVerified)
    , mLastVerified(lastVerified)
{
}

std::string
VerifyLedgerChainWork::getStatus() const
{
    if (mState == WORK_RUNNING)
    {
        std::string task = "verifying checkpoint";
        return fmtProgress(mApp, task, mFirstSeq, mLastSeq, mCurrSeq);
    }
    return Work::getStatus();
}

void
VerifyLedgerChainWork::onReset()
{
    if (mFirstVerified.header.ledgerSeq != 0)
    {
        mFirstVerified = mApp.getLedgerManager().getLastClosedLedgerHeader();
    }
    if (mLastVerified.header.ledgerSeq != 0)
    {
        mLastVerified = mApp.getLedgerManager().getLastClosedLedgerHeader();
    }
    mCurrSeq = mFirstSeq;
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
VerifyLedgerChainWork::verifyHistoryOfSingleCheckpoint()
{
    FileTransferInfo ft(mDownloadDir, HISTORY_FILE_TYPE_LEDGER, mCurrSeq);
    XDRInputFileStream hdrIn;
    hdrIn.open(ft.localPath_nogz());

    LedgerHeaderHistoryEntry prev = mLastVerified;
    LedgerHeaderHistoryEntry curr;

    CLOG(DEBUG, "History") << "Verifying ledger headers from "
                           << ft.localPath_nogz() << " starting from ledger "
                           << LedgerManager::ledgerAbbrev(prev);

    while (hdrIn && hdrIn.readOne(curr))
    {

        if (prev.header.ledgerSeq == 0)
        {
            // When we have no previous state to connect up with
            // (eg. starting somewhere mid-chain like in CATCHUP_MINIMAL)
            // we just accept the first chain entry we see. We will
            // verify the chain continuously from here, and against the
            // live network.
            prev = curr;
            continue;
        }

        uint32_t expectedSeq = prev.header.ledgerSeq + 1;
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
        if (verifyLedgerHistoryLink(prev.hash, curr) !=
            HistoryManager::VERIFY_HASH_OK)
        {
            return HistoryManager::VERIFY_HASH_BAD;
        }
        prev = curr;
    }

    if (curr.header.ledgerSeq != mCurrSeq)
    {
        CLOG(ERROR, "History") << "History chain did not end with " << mCurrSeq;
        return HistoryManager::VERIFY_HASH_BAD;
    }

    auto status = HistoryManager::VERIFY_HASH_OK;
    if (mCurrSeq == mLastSeq)
    {
        CLOG(INFO, "History") << "Verifying catchup candidate " << mCurrSeq
                              << " with LedgerManager";
        status = mApp.getLedgerManager().verifyCatchupCandidate(curr);
        if (status == HistoryManager::VERIFY_HASH_UNKNOWN && mManualCatchup)
        {
            CLOG(WARNING, "History")
                << "Accepting unknown-hash ledger due to manual catchup";
            status = HistoryManager::VERIFY_HASH_OK;
        }
    }

    if (status == HistoryManager::VERIFY_HASH_OK)
    {
        if (mCurrSeq == mFirstSeq)
        {
            mFirstVerified = curr;
        }
        mLastVerified = curr;
    }

    return status;
}

Work::State
VerifyLedgerChainWork::onSuccess()
{
    mApp.getHistoryManager().logAndUpdateStatus(true);

    if (mCurrSeq > mLastSeq)
    {
        throw std::runtime_error("Verification overshot target ledger");
    }

    // This is in onSuccess rather than onRun, so we can force a FAILURE_RAISE.
    switch (verifyHistoryOfSingleCheckpoint())
    {
    case HistoryManager::VERIFY_HASH_OK:
        if (mCurrSeq == mLastSeq)
        {
            CLOG(INFO, "History") << "History chain [" << mFirstSeq << ","
                                  << mLastSeq << "] verified";
            return WORK_SUCCESS;
        }

        mCurrSeq += mApp.getHistoryManager().getCheckpointFrequency();
        return WORK_RUNNING;
    case HistoryManager::VERIFY_HASH_UNKNOWN:
        CLOG(WARNING, "History")
            << "Catchup material verification inconclusive, retrying";
        return WORK_FAILURE_RETRY;
    case HistoryManager::VERIFY_HASH_BAD:
        CLOG(ERROR, "History")
            << "Catchup material failed verification, propagating failure";
        return WORK_FAILURE_RAISE;
    default:
        assert(false);
        throw std::runtime_error("unexpected VerifyLedgerChainWork state");
    }
}

///////////////////////////////////////////////////////////////////////////
// Get / Put HistoryArchiveStates
///////////////////////////////////////////////////////////////////////////

GetHistoryArchiveStateWork::GetHistoryArchiveStateWork(
    Application& app, WorkParent& parent, HistoryArchiveState& state,
    uint32_t seq, VirtualClock::duration const& initialDelay,
    std::shared_ptr<HistoryArchive const> archive, size_t maxRetries)
    : Work(app, parent, "get-history-archive-state", maxRetries)
    , mState(state)
    , mSeq(seq)
    , mInitialDelay(initialDelay)
    , mArchive(archive)
    , mLocalFilename(
          archive ? HistoryArchiveState::localName(app, archive->getName())
                  : app.getHistoryManager().localFilename(
                        HistoryArchiveState::baseName()))
{
}

std::string
GetHistoryArchiveStateWork::getStatus() const
{
    if (getState() == WORK_FAILURE_RETRY)
    {
        auto eta = getRetryETA();
        return fmt::format("Awaiting checkpoint (ETA: {:d} seconds)", eta);
    }
    return Work::getStatus();
}

VirtualClock::duration
GetHistoryArchiveStateWork::getRetryDelay() const
{
    if (mInitialDelay.count() != 0 && mRetries == 0)
    {
        return mInitialDelay;
    }
    return Work::getRetryDelay();
}

void
GetHistoryArchiveStateWork::onReset()
{
    clearChildren();
    std::remove(mLocalFilename.c_str());
    addWork<GetRemoteFileWork>(mSeq == 0
                                   ? HistoryArchiveState::wellKnownRemoteName()
                                   : HistoryArchiveState::remoteName(mSeq),
                               mLocalFilename, mArchive, getMaxRetries());

    if (mSeq != 0 && mRetries == 0 && mInitialDelay.count() != 0)
    {
        // If this is our first reset (on addition) and we're fetching a
        // known snapshot, immediately initiate a timed retry, to avoid
        // cluttering the console with the initial-probe failure.
        setState(WORK_FAILURE_RETRY);
        scheduleRetry();
    }
}

void
GetHistoryArchiveStateWork::onRun()
{
    try
    {
        mState.load(mLocalFilename);
        scheduleSuccess();
    }
    catch (std::runtime_error& e)
    {
        CLOG(ERROR, "History") << "error loading history state: " << e.what();
        scheduleFailure();
    }
}

PutHistoryArchiveStateWork::PutHistoryArchiveStateWork(
    Application& app, WorkParent& parent, HistoryArchiveState const& state,
    std::shared_ptr<HistoryArchive const> archive)
    : Work(app, parent, "put-history-archive-state")
    , mState(state)
    , mArchive(archive)
    , mLocalFilename(HistoryArchiveState::localName(app, archive->getName()))
{
}

void
PutHistoryArchiveStateWork::onReset()
{
    clearChildren();
    mPutRemoteFileWork.reset();
    std::remove(mLocalFilename.c_str());
}

void
PutHistoryArchiveStateWork::onRun()
{
    if (!mPutRemoteFileWork)
    {
        try
        {
            mState.save(mLocalFilename);
            scheduleSuccess();
        }
        catch (std::runtime_error& e)
        {
            CLOG(ERROR, "History") << "error loading history state: "
                                   << e.what();
            scheduleFailure();
        }
    }
    else
    {
        scheduleSuccess();
    }
}

Work::State
PutHistoryArchiveStateWork::onSuccess()
{
    if (!mPutRemoteFileWork)
    {
        // Put the file in the history/ww/xx/yy/history-wwxxyyzz.json file
        auto seqName = HistoryArchiveState::remoteName(mState.currentLedger);
        auto seqDir = HistoryArchiveState::remoteDir(mState.currentLedger);
        mPutRemoteFileWork =
            addWork<PutRemoteFileWork>(mLocalFilename, seqName, mArchive);
        mPutRemoteFileWork->addWork<MakeRemoteDirWork>(seqDir, mArchive);

        // Also put it in the .well-known/stellar-history.json file
        auto wkName = HistoryArchiveState::wellKnownRemoteName();
        auto wkDir = HistoryArchiveState::wellKnownRemoteDir();
        auto wkWork =
            addWork<PutRemoteFileWork>(mLocalFilename, wkName, mArchive);
        wkWork->addWork<MakeRemoteDirWork>(wkDir, mArchive);

        return WORK_PENDING;
    }
    return WORK_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////
// Batch download-and-decompress
///////////////////////////////////////////////////////////////////////////

BatchDownloadWork::BatchDownloadWork(Application& app, WorkParent& parent,
                                     uint32_t first, uint32_t last,
                                     std::string const& type,
                                     TmpDir const& downloadDir)
    : Work(app, parent,
           fmt::format("batch-download-{:s}-{:08x}-{:08x}", type, first, last))
    , mFirst(first)
    , mLast(last)
    , mNext(first)
    , mFileType(type)
    , mDownloadDir(downloadDir)
{
}

std::string
BatchDownloadWork::getStatus() const
{
    if (mState == WORK_RUNNING || mState == WORK_PENDING)
    {
        auto task = fmt::format("downloading {:s} files", mFileType);
        return fmtProgress(mApp, task, mFirst, mLast, mNext);
    }
    return Work::getStatus();
}

void
BatchDownloadWork::addNextDownloadWorker()
{
    if (mNext > mLast)
    {
        return;
    }

    FileTransferInfo ft(mDownloadDir, mFileType, mNext);
    if (fs::exists(ft.localPath_nogz()))
    {
        CLOG(DEBUG, "History") << "already have " << mFileType
                               << " for checkpoint " << mNext;
    }
    else
    {
        CLOG(DEBUG, "History") << "Downloading and unzipping " << mFileType
                               << " for checkpoint " << mNext;
        auto getAndUnzip = addWork<GetAndUnzipRemoteFileWork>(ft);
        assert(mRunning.find(getAndUnzip->getUniqueName()) == mRunning.end());
        mRunning.insert(std::make_pair(getAndUnzip->getUniqueName(), mNext));
    }
    mNext += mApp.getHistoryManager().getCheckpointFrequency();
}

void
BatchDownloadWork::onReset()
{
    mNext = mFirst;
    mRunning.clear();
    mFinished.clear();
    clearChildren();
    size_t nChildren = mApp.getConfig().MAX_CONCURRENT_SUBPROCESSES;
    while (mChildren.size() < nChildren && mNext <= mLast)
    {
        addNextDownloadWorker();
    }
}

void
BatchDownloadWork::notify(std::string const& childChanged)
{
    std::vector<std::string> done;
    for (auto const& c : mChildren)
    {
        if (c.second->getState() == WORK_SUCCESS)
        {
            done.push_back(c.first);
        }
    }
    for (auto const& d : done)
    {
        mChildren.erase(d);
        auto i = mRunning.find(d);
        assert(i != mRunning.end());

        CLOG(DEBUG, "History") << "Finished download of " << mFileType
                               << " for checkpoint " << i->second;

        mFinished.push_back(i->second);
        mRunning.erase(i);
        addNextDownloadWorker();
    }
    mApp.getHistoryManager().logAndUpdateStatus(true);
    advance();
}

///////////////////////////////////////////////////////////////////////////
// Apply Buckets
///////////////////////////////////////////////////////////////////////////

ApplyBucketsWork::ApplyBucketsWork(
    Application& app, WorkParent& parent,
    std::map<std::string, std::shared_ptr<Bucket>>& buckets,
    HistoryArchiveState& applyState,
    LedgerHeaderHistoryEntry const& firstVerified)
    : Work(app, parent, std::string("apply-buckets"))
    , mBuckets(buckets)
    , mApplyState(applyState)
    , mFirstVerified(firstVerified)
    , mApplying(false)
    , mLevel(BucketList::kNumLevels - 1)
{
    // Consistency check: LCL should be in the _past_ from firstVerified,
    // since we're about to clobber a bunch of DB state with new buckets
    // held in firstVerified's state.
    auto lcl = app.getLedgerManager().getLastClosedLedgerHeader();
    if (firstVerified.header.ledgerSeq < lcl.header.ledgerSeq)
    {
        throw std::runtime_error(
            "ApplyBucketsWork applying ledger earlier than local LCL");
    }
}

BucketList&
ApplyBucketsWork::getBucketList()
{
    return mApp.getBucketManager().getBucketList();
}

BucketLevel&
ApplyBucketsWork::getBucketLevel(size_t level)
{
    return getBucketList().getLevel(level);
}

std::shared_ptr<Bucket>
ApplyBucketsWork::getBucket(std::string const& hash)
{
    std::shared_ptr<Bucket> b;
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
ApplyBucketsWork::onReset()
{
    mLevel = BucketList::kNumLevels - 1;
    mApplying = false;
    mSnapBucket.reset();
    mCurrBucket.reset();
    mSnapApplicator.reset();
    mCurrApplicator.reset();
}

void
ApplyBucketsWork::onStart()
{
    auto& level = getBucketLevel(mLevel);
    HistoryStateBucket& i = mApplyState.currentBuckets.at(mLevel);
    if (mApplying || i.snap != binToHex(level.getSnap()->getHash()))
    {
        mSnapBucket = getBucket(i.snap);
        mSnapApplicator =
            make_unique<BucketApplicator>(mApp.getDatabase(), mSnapBucket);
        CLOG(DEBUG, "History") << "ApplyBuckets : starting level[" << mLevel
                               << "].snap = " << i.snap;
        mApplying = true;
    }
    if (mApplying || i.curr != binToHex(level.getCurr()->getHash()))
    {
        mCurrBucket = getBucket(i.curr);
        mCurrApplicator =
            make_unique<BucketApplicator>(mApp.getDatabase(), mCurrBucket);
        CLOG(DEBUG, "History") << "ApplyBuckets : starting level[" << mLevel
                               << "].curr = " << i.curr;
        mApplying = true;
    }
}

void
ApplyBucketsWork::onRun()
{
    if (mSnapApplicator && *mSnapApplicator)
    {
        mSnapApplicator->advance();
    }
    else if (mCurrApplicator && *mCurrApplicator)
    {
        mCurrApplicator->advance();
    }
    scheduleSuccess();
}

Work::State
ApplyBucketsWork::onSuccess()
{
    if ((mSnapApplicator && *mSnapApplicator) ||
        (mCurrApplicator && *mCurrApplicator))
    {
        return WORK_RUNNING;
    }

    auto& level = getBucketLevel(mLevel);
    if (mSnapBucket)
    {
        level.setSnap(mSnapBucket);
    }
    if (mCurrBucket)
    {
        level.setCurr(mCurrBucket);
    }
    mSnapBucket.reset();
    mCurrBucket.reset();
    mSnapApplicator.reset();
    mCurrApplicator.reset();

    HistoryStateBucket& i = mApplyState.currentBuckets.at(mLevel);
    level.setNext(i.next);

    if (mLevel != 0)
    {
        --mLevel;
        CLOG(DEBUG, "History") << "ApplyBuckets : starting next level: "
                               << mLevel;
        return WORK_PENDING;
    }

    CLOG(DEBUG, "History") << "ApplyBuckets : done, restarting merges";
    getBucketList().restartMerges(mApp, mFirstVerified.header.ledgerSeq);
    return WORK_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////
// Apply Ledger Chain
///////////////////////////////////////////////////////////////////////////

ApplyLedgerChainWork::ApplyLedgerChainWork(
    Application& app, WorkParent& parent, TmpDir const& downloadDir,
    uint32_t first, uint32_t last, LedgerHeaderHistoryEntry& lastApplied)
    : Work(app, parent, std::string("apply-ledger-chain"))
    , mDownloadDir(downloadDir)
    , mFirstSeq(first)
    , mCurrSeq(first)
    , mLastSeq(last)
    , mLastApplied(lastApplied)
{
}

std::string
ApplyLedgerChainWork::getStatus() const
{
    if (mState == WORK_RUNNING)
    {
        std::string task = "applying checkpoint";
        return fmtProgress(mApp, task, mFirstSeq, mLastSeq, mCurrSeq);
    }
    return Work::getStatus();
}

void
ApplyLedgerChainWork::onReset()
{
    mLastApplied = mApp.getLedgerManager().getLastClosedLedgerHeader();
    uint32_t step = mApp.getHistoryManager().getCheckpointFrequency();
    auto& lm = mApp.getLedgerManager();
    CLOG(INFO, "History") << "Replaying contents of "
                          << (1 + ((mLastSeq - mFirstSeq) / step))
                          << " transaction-history files from LCL "
                          << LedgerManager::ledgerAbbrev(
                                 lm.getLastClosedLedgerHeader());
    mCurrSeq = mFirstSeq;
    mHdrIn.close();
    mTxIn.close();
}

void
ApplyLedgerChainWork::openCurrentInputFiles()
{
    mHdrIn.close();
    mTxIn.close();
    if (mCurrSeq > mLastSeq)
    {
        return;
    }
    FileTransferInfo hi(mDownloadDir, HISTORY_FILE_TYPE_LEDGER, mCurrSeq);
    FileTransferInfo ti(mDownloadDir, HISTORY_FILE_TYPE_TRANSACTIONS, mCurrSeq);
    CLOG(DEBUG, "History") << "Replaying ledger headers from "
                           << hi.localPath_nogz();
    CLOG(DEBUG, "History") << "Replaying transactions from "
                           << ti.localPath_nogz();
    mHdrIn.open(hi.localPath_nogz());
    mTxIn.open(ti.localPath_nogz());
    mTxHistoryEntry = TransactionHistoryEntry();
}

TxSetFramePtr
ApplyLedgerChainWork::getCurrentTxSet()
{
    auto& lm = mApp.getLedgerManager();
    auto seq = lm.getCurrentLedgerHeader().ledgerSeq;

    do
    {
        if (mTxHistoryEntry.ledgerSeq < seq)
        {
            CLOG(DEBUG, "History") << "Skipping txset for ledger "
                                   << mTxHistoryEntry.ledgerSeq;
        }
        else if (mTxHistoryEntry.ledgerSeq > seq)
        {
            break;
        }
        else
        {
            assert(mTxHistoryEntry.ledgerSeq == seq);
            CLOG(DEBUG, "History") << "Loaded txset for ledger " << seq;
            return std::make_shared<TxSetFrame>(mApp.getNetworkID(),
                                                mTxHistoryEntry.txSet);
        }
    } while (mTxIn && mTxIn.readOne(mTxHistoryEntry));

    CLOG(DEBUG, "History") << "Using empty txset for ledger " << seq;
    return std::make_shared<TxSetFrame>(lm.getLastClosedLedgerHeader().hash);
}

bool
ApplyLedgerChainWork::applyHistoryOfSingleLedger()
{
    LedgerHeaderHistoryEntry hHeader;
    LedgerHeader& header = hHeader.header;

    if (!mHdrIn || !mHdrIn.readOne(hHeader))
    {
        return false;
    }

    auto& lm = mApp.getLedgerManager();

    LedgerHeader const& previousHeader = lm.getLastClosedLedgerHeader().header;

    // If we are >1 before LCL, skip
    if (header.ledgerSeq + 1 < previousHeader.ledgerSeq)
    {
        CLOG(DEBUG, "History") << "Catchup skipping old ledger "
                               << header.ledgerSeq;
        return true;
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
        return true;
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
        return true;
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

    auto txset = getCurrentTxSet();
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

    CLOG(DEBUG, "History") << "LedgerManager LCL:\n"
                           << xdr::xdr_to_string(
                                  lm.getLastClosedLedgerHeader());
    CLOG(DEBUG, "History") << "Replay header:\n" << xdr::xdr_to_string(hHeader);
    if (lm.getLastClosedLedgerHeader().hash != hHeader.hash)
    {
        throw std::runtime_error("replay produced mismatched ledger hash");
    }
    mLastApplied = hHeader;
    return true;
}

void
ApplyLedgerChainWork::onStart()
{
    openCurrentInputFiles();
}

void
ApplyLedgerChainWork::onRun()
{
    try
    {
        if (!applyHistoryOfSingleLedger())
        {
            mCurrSeq += mApp.getHistoryManager().getCheckpointFrequency();
            openCurrentInputFiles();
        }
        scheduleSuccess();
    }
    catch (std::runtime_error& e)
    {
        CLOG(ERROR, "History") << "Replay failed: " << e.what();
        scheduleFailure();
    }
}

Work::State
ApplyLedgerChainWork::onSuccess()
{
    if (mCurrSeq > mLastSeq)
    {
        return WORK_SUCCESS;
    }
    return WORK_RUNNING;
}

///////////////////////////////////////////////////////////////////////////
// Get and unzip
///////////////////////////////////////////////////////////////////////////

GetAndUnzipRemoteFileWork::GetAndUnzipRemoteFileWork(
    Application& app, WorkParent& parent, FileTransferInfo ft,
    std::shared_ptr<HistoryArchive const> archive, size_t maxRetries)
    : Work(app, parent,
           std::string("get-and-unzip-remote-file ") + ft.remoteName(),
           maxRetries)
    , mFt(std::move(ft))
    , mArchive(archive)
{
}

std::string
GetAndUnzipRemoteFileWork::getStatus() const
{
    if (mState == WORK_PENDING)
    {
        if (mGunzipFileWork)
        {
            return mGunzipFileWork->getStatus();
        }
        else if (mGetRemoteFileWork)
        {
            return mGetRemoteFileWork->getStatus();
        }
    }
    return Work::getStatus();
}

void
GetAndUnzipRemoteFileWork::onReset()
{
    clearChildren();
    mGetRemoteFileWork.reset();
    mGunzipFileWork.reset();

    CLOG(DEBUG, "History") << "Downloading and unzipping " << mFt.remoteName()
                           << ": downloading";
    mGetRemoteFileWork =
        addWork<GetRemoteFileWork>(mFt.remoteName(), mFt.localPath_gz_tmp());
}

Work::State
GetAndUnzipRemoteFileWork::onSuccess()
{
    if (mGunzipFileWork)
    {
        if (!fs::exists(mFt.localPath_nogz()))
        {
            CLOG(ERROR, "History") << "Downloading and unzipping "
                                   << mFt.remoteName() << ": .xdr not found";
            return WORK_FAILURE_RETRY;
        }
        else
        {
            return WORK_SUCCESS;
        }
    }

    if (fs::exists(mFt.localPath_gz_tmp()))
    {
        CLOG(TRACE, "History") << "Downloading and unzipping "
                               << mFt.remoteName()
                               << ": renaming .gz.tmp to .gz";
        if (fs::exists(mFt.localPath_gz()) &&
            std::remove(mFt.localPath_gz().c_str()))
        {
            CLOG(ERROR, "History") << "Downloading and unzipping "
                                   << mFt.remoteName()
                                   << ": failed to remove .gz";
            return WORK_FAILURE_RETRY;
        }

        if (std::rename(mFt.localPath_gz_tmp().c_str(),
                        mFt.localPath_gz().c_str()))
        {
            CLOG(ERROR, "History") << "Downloading and unzipping "
                                   << mFt.remoteName()
                                   << ": failed to rename .gz.tmp to .gz";
            return WORK_FAILURE_RETRY;
        }

        CLOG(TRACE, "History") << "Downloading and unzipping "
                               << mFt.remoteName()
                               << ": renamed .gz.tmp to .gz";
    }

    if (!fs::exists(mFt.localPath_gz()))
    {
        CLOG(ERROR, "History") << "Downloading and unzipping "
                               << mFt.remoteName() << ": .gz not found";
        return WORK_FAILURE_RETRY;
    }

    CLOG(DEBUG, "History") << "Downloading and unzipping " << mFt.remoteName()
                           << ": unzipping";
    mGunzipFileWork = addWork<GunzipFileWork>(mFt.localPath_gz(), false, 1);
    return WORK_PENDING;
}

void
GetAndUnzipRemoteFileWork::onFailureRaise()
{
    std::remove(mFt.localPath_nogz().c_str());
    std::remove(mFt.localPath_gz().c_str());
    std::remove(mFt.localPath_gz_tmp().c_str());
}

///////////////////////////////////////////////////////////////////////////
// Base class for Catchup and Repair
///////////////////////////////////////////////////////////////////////////

BucketDownloadWork::BucketDownloadWork(Application& app, WorkParent& parent,
                                       std::string const& uniqueName,
                                       HistoryArchiveState const& localState)
    : Work(app, parent, uniqueName)
    , mLocalState(localState)
    , mDownloadDir(
          make_unique<TmpDir>(mApp.getTmpDirManager().tmpDir(getUniqueName())))
{
}

void
BucketDownloadWork::onReset()
{
    clearChildren();
    mBuckets.clear();
}

void
BucketDownloadWork::takeDownloadDir(BucketDownloadWork& other)
{
    if (other.mDownloadDir)
    {
        mDownloadDir = std::move(other.mDownloadDir);
    }
}

///////////////////////////////////////////////////////////////////////////
// Catchup
///////////////////////////////////////////////////////////////////////////

CatchupWork::CatchupWork(Application& app, WorkParent& parent,
                         uint32_t initLedger, std::string const& mode,
                         bool manualCatchup)
    : BucketDownloadWork(
          app, parent, fmt::format("catchup-{:s}-{:08x}", mode, initLedger),
          app.getHistoryManager().getLastClosedHistoryArchiveState())
    , mInitLedger(initLedger)
    , mManualCatchup(manualCatchup)
{
}

uint32_t
CatchupWork::nextLedger() const
{
    return mManualCatchup
               ? mInitLedger
               : mApp.getHistoryManager().nextCheckpointLedger(mInitLedger);
}

uint32_t
CatchupWork::lastCheckpointSeq() const
{
    return nextLedger() - 1;
}

void
CatchupWork::onReset()
{
    BucketDownloadWork::onReset();
    uint64_t sleepSeconds =
        mManualCatchup ? 0
                       : mApp.getHistoryManager().nextCheckpointCatchupProbe(
                             lastCheckpointSeq());
    mGetHistoryArchiveStateWork = addWork<GetHistoryArchiveStateWork>(
        mRemoteState, firstCheckpointSeq(), std::chrono::seconds(sleepSeconds));
}

///////////////////////////////////////////////////////////////////////////
// Catchup Minimal
///////////////////////////////////////////////////////////////////////////

CatchupMinimalWork::CatchupMinimalWork(Application& app, WorkParent& parent,
                                       uint32_t initLedger, bool manualCatchup,
                                       handler endHandler)
    : CatchupWork(app, parent, initLedger, "minimal", manualCatchup)
    , mEndHandler(endHandler)
{
}

uint32_t
CatchupMinimalWork::firstCheckpointSeq() const
{
    auto firstLedger = mInitLedger > mApp.getConfig().CATCHUP_RECENT
                           ? (mInitLedger - mApp.getConfig().CATCHUP_RECENT)
                           : 0;
    return mApp.getHistoryManager().nextCheckpointLedger(firstLedger) - 1;
}

std::string
CatchupMinimalWork::getStatus() const
{
    if (mState == WORK_PENDING)
    {
        if (mApplyWork)
        {
            return mApplyWork->getStatus();
        }
        else if (mDownloadBucketsWork)
        {
            return mDownloadBucketsWork->getStatus();
        }
        else if (mVerifyLedgersWork)
        {
            return mVerifyLedgersWork->getStatus();
        }
        else if (mDownloadLedgersWork)
        {
            return mDownloadLedgersWork->getStatus();
        }
        else if (mGetHistoryArchiveStateWork)
        {
            return mGetHistoryArchiveStateWork->getStatus();
        }
    }
    return Work::getStatus();
}

void
CatchupMinimalWork::onReset()
{
    CatchupWork::onReset();
    mDownloadLedgersWork.reset();
    mVerifyLedgersWork.reset();
    mDownloadBucketsWork.reset();
    mApplyWork.reset();
}

Work::State
CatchupMinimalWork::onSuccess()
{
    // Phase 1 starts automatically in base class: CatchupWork::onReset
    // If we get here, phase 1 should be complete and we're moving on.
    assert(mGetHistoryArchiveStateWork);
    assert(mGetHistoryArchiveStateWork->getState() == WORK_SUCCESS);

    auto firstSeq = firstCheckpointSeq();
    auto lastSeq = lastCheckpointSeq();

    // Phase 2: download the ledger chain that validates the state
    // we're about to assume.
    if (!mDownloadLedgersWork)
    {
        CLOG(INFO, "History") << "Catchup MINIMAL downloading ledger chain";
        mDownloadLedgersWork = addWork<BatchDownloadWork>(
            firstSeq, lastSeq, HISTORY_FILE_TYPE_LEDGER, *mDownloadDir);
        return WORK_PENDING;
    }

    // Phase 3: Verify the ledger chain
    if (!mVerifyLedgersWork)
    {
        CLOG(INFO, "History") << "Catchup MINIMAL verifying ledger chain";
        mVerifyLedgersWork = addWork<VerifyLedgerChainWork>(
            *mDownloadDir, firstSeq, lastSeq, mManualCatchup, mFirstVerified,
            mLastVerified);
        return WORK_PENDING;
    }

    // Phase 4: download and verify the buckets themselves.
    if (!mDownloadBucketsWork)
    {
        CLOG(INFO, "History")
            << "Catchup MINIMAL downloading and verifying buckets";
        std::vector<std::string> buckets =
            mRemoteState.differingBuckets(mLocalState);
        mDownloadBucketsWork = addWork<Work>("download and verify buckets");
        for (auto const& hash : buckets)
        {
            FileTransferInfo ft(*mDownloadDir, HISTORY_FILE_TYPE_BUCKET, hash);
            // Each bucket gets its own work-chain of download->gunzip->verify

            auto verify = mDownloadBucketsWork->addWork<VerifyBucketWork>(
                mBuckets, ft.localPath_nogz(), hexToBin256(hash));
            verify->addWork<GetAndUnzipRemoteFileWork>(ft);
        }
        return WORK_PENDING;
    }

    assert(mDownloadLedgersWork->getState() == WORK_SUCCESS);
    assert(mVerifyLedgersWork->getState() == WORK_SUCCESS);
    assert(mDownloadBucketsWork->getState() == WORK_SUCCESS);

    // Phase 3: apply the buckets.
    if (!mApplyWork)
    {
        CLOG(INFO, "History") << "Catchup MINIMAL applying buckets for state "
                              << LedgerManager::ledgerAbbrev(mFirstVerified);
        mApplyWork =
            addWork<ApplyBucketsWork>(mBuckets, mRemoteState, mFirstVerified);
        return WORK_PENDING;
    }

    CLOG(INFO, "History") << "Completed catchup MINIMAL to state "
                          << LedgerManager::ledgerAbbrev(mFirstVerified)
                          << " for nextLedger=" << nextLedger();
    mApp.getHistoryManager().historyCaughtup();
    asio::error_code ec;
    mEndHandler(ec, HistoryManager::CATCHUP_MINIMAL, mFirstVerified);

    return WORK_SUCCESS;
}

void
CatchupMinimalWork::onFailureRaise()
{
    mApp.getHistoryManager().historyCaughtup();
    asio::error_code ec = std::make_error_code(std::errc::timed_out);
    mEndHandler(ec, HistoryManager::CATCHUP_MINIMAL, mLastVerified);
}

///////////////////////////////////////////////////////////////////////////
// Catchup Complete
///////////////////////////////////////////////////////////////////////////

CatchupCompleteWork::CatchupCompleteWork(Application& app, WorkParent& parent,
                                         uint32_t initLedger,
                                         bool manualCatchup, handler endHandler)
    : CatchupWork(app, parent, initLedger, "complete", manualCatchup)
    , mEndHandler(endHandler)
{
}

std::string
CatchupCompleteWork::getStatus() const
{
    if (mState == WORK_PENDING)
    {
        if (mApplyWork)
        {
            return mApplyWork->getStatus();
        }
        else if (mVerifyWork)
        {
            return mVerifyWork->getStatus();
        }
        else if (mDownloadTransactionsWork)
        {
            return mDownloadTransactionsWork->getStatus();
        }
        else if (mDownloadLedgersWork)
        {
            return mDownloadLedgersWork->getStatus();
        }
        else if (mGetHistoryArchiveStateWork)
        {
            return mGetHistoryArchiveStateWork->getStatus();
        }
    }
    return Work::getStatus();
}

uint32_t
CatchupCompleteWork::firstCheckpointSeq() const
{

    auto firstLedger = mLocalState.currentLedger;
    return mApp.getHistoryManager().nextCheckpointLedger(firstLedger) - 1;
}

void
CatchupCompleteWork::onReset()
{
    CatchupWork::onReset();
    mDownloadLedgersWork.reset();
    mDownloadTransactionsWork.reset();
    mVerifyWork.reset();
    mApplyWork.reset();
}

Work::State
CatchupCompleteWork::onSuccess()
{
    // Phase 1 starts automatically in base class: CatchupWork::onReset
    // If we get here, phase 1 should be complete and we're moving on.
    assert(mGetHistoryArchiveStateWork);
    assert(mGetHistoryArchiveStateWork->getState() == WORK_SUCCESS);

    uint32_t firstSeq = firstCheckpointSeq();
    uint32_t lastSeq = lastCheckpointSeq();

    // Phase 2: download and decompress the ledgers.
    if (!mDownloadLedgersWork)
    {
        CLOG(INFO, "History") << "Catchup COMPLETE downloading ledgers ["
                              << firstSeq << ", " << lastSeq << "]";
        mDownloadLedgersWork = addWork<BatchDownloadWork>(
            firstSeq, lastSeq, HISTORY_FILE_TYPE_LEDGER, *mDownloadDir);
        return WORK_PENDING;
    }

    // Phase 3: download and decompress the transactions.
    if (!mDownloadTransactionsWork)
    {
        CLOG(INFO, "History") << "Catchup COMPLETE downloading transactions";
        mDownloadTransactionsWork = addWork<BatchDownloadWork>(
            firstSeq, lastSeq, HISTORY_FILE_TYPE_TRANSACTIONS, *mDownloadDir);
        return WORK_PENDING;
    }

    // Phase 4: verify the ledger chain.
    if (!mVerifyWork)
    {
        CLOG(INFO, "History") << "Catchup COMPLETE verifying history";
        mLastVerified = mApp.getLedgerManager().getLastClosedLedgerHeader();
        mVerifyWork = addWork<VerifyLedgerChainWork>(
            *mDownloadDir, firstSeq, lastSeq, mManualCatchup, mFirstVerified,
            mLastVerified);
        return WORK_PENDING;
    }

    // Phase 5: apply the transactions.
    if (!mApplyWork)
    {
        CLOG(INFO, "History") << "Catchup COMPLETE applying history";
        mApplyWork = addWork<ApplyLedgerChainWork>(*mDownloadDir, firstSeq,
                                                   lastSeq, mLastApplied);
        return WORK_PENDING;
    }

    CLOG(INFO, "History") << "Completed catchup COMPLETE to state "
                          << LedgerManager::ledgerAbbrev(mLastApplied)
                          << " for nextLedger=" << nextLedger();
    mApp.getHistoryManager().historyCaughtup();
    asio::error_code ec;
    mEndHandler(ec, HistoryManager::CATCHUP_COMPLETE, mLastApplied);

    return WORK_SUCCESS;
}

void
CatchupCompleteWork::onFailureRaise()
{
    mApp.getHistoryManager().historyCaughtup();
    asio::error_code ec = std::make_error_code(std::errc::timed_out);
    mEndHandler(ec, HistoryManager::CATCHUP_COMPLETE, mLastVerified);
}

///////////////////////////////////////////////////////////////////////////
// Shapsnot resolve / write-out
///////////////////////////////////////////////////////////////////////////

ResolveSnapshotWork::ResolveSnapshotWork(
    Application& app, WorkParent& parent,
    std::shared_ptr<StateSnapshot> snapshot)
    : Work(app, parent, "prepare-snapshot", Work::RETRY_FOREVER)
    , mSnapshot(snapshot)
{
}

void
ResolveSnapshotWork::onRun()
{
    mSnapshot->mLocalState.resolveAnyReadyFutures();
    mSnapshot->makeLive();
    if (mApp.getLedgerManager().getState() == LedgerManager::LM_SYNCED_STATE &&
        mSnapshot->mLocalState.futuresAllResolved())
    {
        scheduleSuccess();
    }
    else
    {
        scheduleFailure();
    }
}

WriteSnapshotWork::WriteSnapshotWork(Application& app, WorkParent& parent,
                                     std::shared_ptr<StateSnapshot> snapshot)
    : Work(app, parent, "write-snapshot", Work::RETRY_A_LOT)
    , mSnapshot(snapshot)
{
}

void
WriteSnapshotWork::onStart()
{
    auto handler = callComplete();
    auto snap = mSnapshot;
    auto work = [handler, snap]() {
        asio::error_code ec;
        if (!snap->writeHistoryBlocks())
        {
            ec = std::make_error_code(std::errc::io_error);
        }
        snap->mApp.getClock().getIOService().post(
            [handler, ec]() { handler(ec); });
    };

    // Throw the work over to a worker thread if we can use DB pools,
    // otherwise run on main thread.
    if (mApp.getDatabase().canUsePool())
    {
        mApp.getWorkerIOService().post(work);
    }
    else
    {
        work();
    }
}

void
WriteSnapshotWork::onRun()
{
    // Do nothing: we spawned the writer in onStart().
}

///////////////////////////////////////////////////////////////////////////
// Publish
///////////////////////////////////////////////////////////////////////////

PutSnapshotFilesWork::PutSnapshotFilesWork(
    Application& app, WorkParent& parent,
    std::shared_ptr<HistoryArchive const> archive,
    std::shared_ptr<StateSnapshot> snapshot)
    : Work(app, parent, "put-snapshot-files")
    , mArchive(archive)
    , mSnapshot(snapshot)
{
}

void
PutSnapshotFilesWork::onReset()
{
    clearChildren();

    mGetHistoryArchiveStateWork.reset();
    mPutFilesWork.reset();
    mPutHistoryArchiveStateWork.reset();
}

Work::State
PutSnapshotFilesWork::onSuccess()
{
    // Phase 1: fetch remote history archive state
    if (!mGetHistoryArchiveStateWork)
    {
        mGetHistoryArchiveStateWork = addWork<GetHistoryArchiveStateWork>(
            mRemoteState, 0, std::chrono::seconds(0), mArchive);
        return WORK_PENDING;
    }

    // Phase 2: put all requisite data files
    if (!mPutFilesWork)
    {
        mPutFilesWork = addWork<Work>("put-files");

        std::vector<std::shared_ptr<FileTransferInfo>> files = {
            mSnapshot->mLedgerSnapFile, mSnapshot->mTransactionSnapFile,
            mSnapshot->mTransactionResultSnapFile,
            mSnapshot->mSCPHistorySnapFile};

        std::vector<std::string> bucketsToSend =
            mSnapshot->mLocalState.differingBuckets(mRemoteState);

        for (auto const& hash : bucketsToSend)
        {
            auto b = mApp.getBucketManager().getBucketByHash(hexToBin256(hash));
            assert(b);
            files.push_back(std::make_shared<FileTransferInfo>(*b));
        }
        for (auto f : files)
        {
            if (f && fs::exists(f->localPath_nogz()))
            {
                auto put = mPutFilesWork->addWork<PutRemoteFileWork>(
                    f->localPath_gz(), f->remoteName(), mArchive);
                auto mkdir =
                    put->addWork<MakeRemoteDirWork>(f->remoteDir(), mArchive);
                mkdir->addWork<GzipFileWork>(f->localPath_nogz(), true);
            }
        }
        return WORK_PENDING;
    }

    // Phase 3: update remote history archive state
    if (!mPutHistoryArchiveStateWork)
    {
        mPutHistoryArchiveStateWork = addWork<PutHistoryArchiveStateWork>(
            mSnapshot->mLocalState, mArchive);
        return WORK_PENDING;
    }

    return WORK_SUCCESS;
}

PublishWork::PublishWork(Application& app, WorkParent& parent,
                         std::shared_ptr<StateSnapshot> snapshot)
    : Work(app, parent,
           fmt::format("publish-{:08x}", snapshot->mLocalState.currentLedger))
    , mSnapshot(snapshot)
{
}

std::string
PublishWork::getStatus() const
{
    if (mState == WORK_PENDING)
    {
        if (mResolveSnapshotWork)
        {
            return mResolveSnapshotWork->getStatus();
        }
        else if (mWriteSnapshotWork)
        {
            return mWriteSnapshotWork->getStatus();
        }
        else if (mUpdateArchivesWork)
        {
            return mUpdateArchivesWork->getStatus();
        }
    }
    return Work::getStatus();
}

void
PublishWork::onReset()
{
    clearChildren();

    mResolveSnapshotWork.reset();
    mWriteSnapshotWork.reset();
    mUpdateArchivesWork.reset();
}

Work::State
PublishWork::onSuccess()
{
    // Phase 1: resolve futures in snapshot
    if (!mResolveSnapshotWork)
    {
        mResolveSnapshotWork = addWork<ResolveSnapshotWork>(mSnapshot);
        return WORK_PENDING;
    }

    // Phase 2: write snapshot files
    if (!mWriteSnapshotWork)
    {
        mWriteSnapshotWork = addWork<WriteSnapshotWork>(mSnapshot);
        return WORK_PENDING;
    }

    // Phase 3: update archives
    if (!mUpdateArchivesWork)
    {
        mUpdateArchivesWork = addWork<Work>("update-archives");
        for (auto& aPair : mApp.getConfig().HISTORY)
        {
            auto arch = aPair.second;
            if (!arch->hasPutCmd())
            {
                continue;
            }
            mUpdateArchivesWork->addWork<PutSnapshotFilesWork>(arch, mSnapshot);
        }
        return WORK_PENDING;
    }

    mApp.getHistoryManager().historyPublished(
        mSnapshot->mLocalState.currentLedger, true);
    return WORK_SUCCESS;
}

void
PublishWork::onFailureRaise()
{
    mApp.getHistoryManager().historyPublished(
        mSnapshot->mLocalState.currentLedger, false);
}

///////////////////////////////////////////////////////////////////////////
// Missing bucket repair
///////////////////////////////////////////////////////////////////////////

RepairMissingBucketsWork::RepairMissingBucketsWork(
    Application& app, WorkParent& parent, HistoryArchiveState const& localState,
    handler endHandler)
    : BucketDownloadWork(app, parent, "repair-buckets", localState)
    , mEndHandler(endHandler)
{
}

void
RepairMissingBucketsWork::onReset()
{
    BucketDownloadWork::onReset();
    std::unordered_set<std::string> bucketsToFetch;
    auto missingBuckets =
        mApp.getBucketManager().checkForMissingBucketsFiles(mLocalState);
    auto publishBuckets =
        mApp.getHistoryManager().getMissingBucketsReferencedByPublishQueue();

    bucketsToFetch.insert(missingBuckets.begin(), missingBuckets.end());
    bucketsToFetch.insert(publishBuckets.begin(), publishBuckets.end());

    for (auto const& hash : bucketsToFetch)
    {
        FileTransferInfo ft(*mDownloadDir, HISTORY_FILE_TYPE_BUCKET, hash);
        // Each bucket gets its own work-chain of download->gunzip->verify
        auto verify = addWork<VerifyBucketWork>(mBuckets, ft.localPath_nogz(),
                                                hexToBin256(hash));
        verify->addWork<GetAndUnzipRemoteFileWork>(ft);
    }
}

Work::State
RepairMissingBucketsWork::onSuccess()
{
    asio::error_code ec;
    mEndHandler(ec);
    return WORK_SUCCESS;
}

void
RepairMissingBucketsWork::onFailureRaise()
{
    asio::error_code ec = std::make_error_code(std::errc::io_error);
    mEndHandler(ec);
}

///////////////////////////////////////////////////////////////////////////
// CatchupRecentWork
///////////////////////////////////////////////////////////////////////////

CatchupRecentWork::CatchupRecentWork(Application& app, WorkParent& parent,
                                     uint32_t initLedger, bool manualCatchup,
                                     handler endHandler)
    : Work(app, parent, fmt::format("catchup-recent-{:d}-from-{:08x}",
                                    app.getConfig().CATCHUP_RECENT, initLedger))
    , mInitLedger(initLedger)
    , mManualCatchup(manualCatchup)
    , mEndHandler(endHandler)
{
}

std::string
CatchupRecentWork::getStatus() const
{
    if (mState == WORK_PENDING)
    {
        if (mCatchupCompleteWork)
        {
            return mCatchupCompleteWork->getStatus();
        }
        if (mCatchupMinimalWork)
        {
            return mCatchupMinimalWork->getStatus();
        }
    }
    return Work::getStatus();
}

void
CatchupRecentWork::onReset()
{
    clearChildren();
    mCatchupMinimalWork.reset();
    mCatchupCompleteWork.reset();
    mFirstVerified = LedgerHeaderHistoryEntry();
    mLastApplied = LedgerHeaderHistoryEntry();
}

CatchupRecentWork::handler
CatchupRecentWork::writeFirstVerified()
{
    std::weak_ptr<CatchupRecentWork> weak =
        std::static_pointer_cast<CatchupRecentWork>(shared_from_this());
    return [weak](asio::error_code const& ec, HistoryManager::CatchupMode mode,
                  LedgerHeaderHistoryEntry const& ledger) {
        auto self = weak.lock();
        if (!self)
        {
            return;
        }
        self->mFirstVerified = ledger;
    };
}

CatchupRecentWork::handler
CatchupRecentWork::writeLastApplied()
{
    std::weak_ptr<CatchupRecentWork> weak =
        std::static_pointer_cast<CatchupRecentWork>(shared_from_this());
    return [weak](asio::error_code const& ec, HistoryManager::CatchupMode mode,
                  LedgerHeaderHistoryEntry const& ledger) {
        auto self = weak.lock();
        if (!self)
        {
            return;
        }
        self->mLastApplied = ledger;
    };
}

Work::State
CatchupRecentWork::onSuccess()
{
    if (!mCatchupMinimalWork)
    {
        CLOG(INFO, "History")
            << "CATCHUP_RECENT starting inner CATCHUP_MINIMAL";
        mCatchupMinimalWork = addWork<CatchupMinimalWork>(
            mInitLedger, mManualCatchup, writeFirstVerified());
        return WORK_PENDING;
    }

    if (!mCatchupCompleteWork)
    {
        CLOG(INFO, "History")
            << "CATCHUP_RECENT finished inner CATCHUP_MINIMAL";
        assert(mCatchupMinimalWork &&
               mCatchupMinimalWork->getState() == WORK_SUCCESS);
        // We make an initial callback in mode CATCHUP_RECENT, to drive the
        // CATCHUP_MINIMAL LCL we just got through to the LM, and prepare
        // it for the upcoming CATCHUP_COMPLETE replay.
        asio::error_code ec;
        mEndHandler(ec, HistoryManager::CATCHUP_RECENT, mFirstVerified);

        CLOG(INFO, "History")
            << "CATCHUP_RECENT starting inner CATCHUP_COMPLETE";
        // Now make a CATCHUP_COMPLETE inner worker, for replay.
        mCatchupCompleteWork = addWork<CatchupCompleteWork>(
            mInitLedger, mManualCatchup, writeLastApplied());

        // Transfer the download dir used by the minimal catchup to the
        // complete catchup, to avoid re-downloading the ledger history.
        auto minimal =
            std::static_pointer_cast<CatchupMinimalWork>(mCatchupMinimalWork);
        auto complete =
            std::static_pointer_cast<CatchupCompleteWork>(mCatchupCompleteWork);
        complete->takeDownloadDir(*minimal);

        return WORK_PENDING;
    }

    assert(mCatchupMinimalWork &&
           mCatchupMinimalWork->getState() == WORK_SUCCESS);
    assert(mCatchupCompleteWork &&
           mCatchupCompleteWork->getState() == WORK_SUCCESS);

    CLOG(INFO, "History") << "CATCHUP_RECENT finished inner CATCHUP_COMPLETE";
    // The second callback we make is CATCHUP_COMPLETE
    mApp.getHistoryManager().historyCaughtup();
    asio::error_code ec;
    mEndHandler(ec, HistoryManager::CATCHUP_COMPLETE, mLastApplied);
    return WORK_SUCCESS;
}

void
CatchupRecentWork::onFailureRaise()
{
    mApp.getHistoryManager().historyCaughtup();
    asio::error_code ec = std::make_error_code(std::errc::timed_out);
    mEndHandler(ec, HistoryManager::CATCHUP_RECENT, mFirstVerified);
}

///////////////////////////////////////////////////////////////////////////
// FetchRecentQsetsWork
///////////////////////////////////////////////////////////////////////////
FetchRecentQsetsWork::FetchRecentQsetsWork(Application& app, WorkParent& parent,
                                           InferredQuorum& inferredQuorum,
                                           handler endHandler)
    : Work(app, parent, "fetch-recent-qsets")
    , mEndHandler(endHandler)
    , mInferredQuorum(inferredQuorum)
{
}

void
FetchRecentQsetsWork::onReset()
{
    clearChildren();
    mDownloadSCPMessagesWork.reset();
    mDownloadDir =
        make_unique<TmpDir>(mApp.getTmpDirManager().tmpDir(getUniqueName()));
}

void
FetchRecentQsetsWork::onFailureRaise()
{
    asio::error_code ec = std::make_error_code(std::errc::timed_out);
    mEndHandler(ec);
}

Work::State
FetchRecentQsetsWork::onSuccess()
{
    // Phase 1: fetch remote history archive state
    if (!mGetHistoryArchiveStateWork)
    {
        mGetHistoryArchiveStateWork = addWork<GetHistoryArchiveStateWork>(
            mRemoteState, 0, std::chrono::seconds(0));
        return WORK_PENDING;
    }

    // Phase 2: download some SCP messages; for now we just pull the past
    // 100 checkpoints = 9 hours of history. A more sophisticated view
    // would survey longer time periods at lower resolution.
    uint32_t numCheckpoints = 100;
    uint32_t step = mApp.getHistoryManager().getCheckpointFrequency();
    uint32_t window = numCheckpoints * step;
    uint32_t lastSeq = mRemoteState.currentLedger;
    uint32_t firstSeq = lastSeq < window ? (step - 1) : (lastSeq - window);

    if (!mDownloadSCPMessagesWork)
    {
        CLOG(INFO, "History") << "Downloading recent SCP messages: ["
                              << firstSeq << ", " << lastSeq << "]";
        mDownloadSCPMessagesWork = addWork<BatchDownloadWork>(
            firstSeq, lastSeq, HISTORY_FILE_TYPE_SCP, *mDownloadDir);
        return WORK_PENDING;
    }

    // Phase 3: extract the qsets.
    // use uint64_t for i to prevent overflows
    for (uint64_t i = firstSeq; i <= lastSeq; i += step)
    {
        CLOG(INFO, "History") << "Scanning for QSets in checkpoint: " << i;
        XDRInputFileStream in;
        FileTransferInfo fi(*mDownloadDir, HISTORY_FILE_TYPE_SCP, i);
        in.open(fi.localPath_nogz());
        SCPHistoryEntry tmp;
        while (in && in.readOne(tmp))
        {
            mInferredQuorum.noteSCPHistory(tmp);
        }
    }

    asio::error_code ec;
    mEndHandler(ec);
    return WORK_SUCCESS;
}
}
