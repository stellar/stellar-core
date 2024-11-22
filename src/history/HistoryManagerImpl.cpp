// Copyright 2014-2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"

#include "bucket/BucketList.h"
#include "bucket/BucketManager.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "herder/HerderImpl.h"
#include "history/HistoryArchive.h"
#include "history/HistoryArchiveManager.h"
#include "history/HistoryManagerImpl.h"
#include "history/StateSnapshot.h"
#include "historywork/FetchRecentQsetsWork.h"
#include "historywork/PublishWork.h"
#include "historywork/PutSnapshotFilesWork.h"
#include "historywork/ResolveSnapshotWork.h"
#include "historywork/WriteSnapshotWork.h"
#include "ledger/LedgerHeaderUtils.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "main/Config.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "overlay/StellarXDR.h"
#include "process/ProcessManager.h"
#include "transactions/TransactionSQL.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/StatusManager.h"
#include "util/TmpDir.h"
#include "work/ConditionalWork.h"
#include "work/WorkScheduler.h"
#include "xdrpp/marshal.h"
#include <Tracy.hpp>
#include <fmt/format.h>

#include <fstream>
#include <regex>
#include <system_error>

namespace stellar
{

using namespace std;

static std::string kSQLCreateStatement =
    "CREATE TABLE IF NOT EXISTS publishqueue ("
    "ledger   INTEGER PRIMARY KEY,"
    "state    TEXT"
    "); ";

void
HistoryManager::dropAll(Database& db)
{
    db.getRawSession() << "DROP TABLE IF EXISTS publishqueue;";
    soci::statement st = db.getRawSession().prepare << kSQLCreateStatement;
    st.execute(true);
}

std::filesystem::path
HistoryManager::publishQueuePath(Config const& cfg)
{
    std::filesystem::path b = cfg.BUCKET_DIR_PATH;
    return b / "publishqueue";
}

void
HistoryManager::createPublishQueueDir(Config const& cfg)
{
    fs::mkpath(HistoryManager::publishQueuePath(cfg).string());
}

std::filesystem::path
publishQueueFileName(uint32_t seq)
{
    return fs::hexStr(seq) + ".json";
}

std::filesystem::path
publishQueueTmpFileName(uint32_t seq)
{
    return fs::hexStr(seq) + ".json.dirty";
}

void
writeCheckpointFile(Application& app, HistoryArchiveState const& has,
                    bool finalize)
{
    releaseAssert(
        app.getHistoryManager().isLastLedgerInCheckpoint(has.currentLedger));
    auto filename = publishQueueFileName(has.currentLedger);
    auto tmpOut = app.getHistoryManager().getTmpDir() / filename;
    has.save(tmpOut.string());

    // Immediately produce a final checkpoint JSON (suitable for confirmed
    // ledgers)
    if (finalize)
    {
        auto out = HistoryManager::publishQueuePath(app.getConfig()) / filename;
        // Durable rename also fsyncs the file which ensures durability
        fs::durableRename(
            tmpOut.string(), out.string(),
            HistoryManager::publishQueuePath(app.getConfig()).string());
    }
    else
    {
        auto out = HistoryManager::publishQueuePath(app.getConfig()) /
                   publishQueueTmpFileName(has.currentLedger);
        // Otherwise, white a temporary durable file, to be finalized once
        // has.currentLedger is actually committed
        fs::durableRename(
            tmpOut.string(), out.string(),
            HistoryManager::publishQueuePath(app.getConfig()).string());
    }
}

void
HistoryManagerImpl::dropSQLBasedPublish()
{
    if (!mApp.getHistoryArchiveManager().publishEnabled())
    {
        // Publish is disabled, nothing to do
        return;
    }

    // soci::transaction is created externally during schema upgrade, so this
    // function is atomic
    releaseAssert(threadIsMain());

    auto const& cfg = mApp.getConfig();
    auto& db = mApp.getDatabase();
    auto& sess = db.getSession();

    // In case previous schema migration rolled back, cleanup files
    fs::deltree(publishQueuePath(cfg).string());
    fs::deltree(getPublishHistoryDir(FileType::HISTORY_FILE_TYPE_LEDGER, cfg)
                    .parent_path()
                    .string());
    createPublishDir(FileType::HISTORY_FILE_TYPE_LEDGER, cfg);
    createPublishDir(FileType::HISTORY_FILE_TYPE_TRANSACTIONS, cfg);
    createPublishDir(FileType::HISTORY_FILE_TYPE_RESULTS, cfg);
    HistoryManager::createPublishQueueDir(cfg);

    std::set<uint32_t> checkpointLedgers;
    // Migrate all the existing queued checkpoints to the new format
    {
        std::string state;
        auto prep =
            db.getPreparedStatement("SELECT state FROM publishqueue;", sess);
        auto& st = prep.statement();
        st.exchange(soci::into(state));
        st.define_and_bind();
        st.execute(true);
        while (st.got_data())
        {
            HistoryArchiveState has;
            has.fromString(state);
            releaseAssert(isLastLedgerInCheckpoint(has.currentLedger));
            checkpointLedgers.insert(has.currentLedger);
            writeCheckpointFile(mApp, has, /* finalize */ true);
            st.fetch();
        }
    }

    auto freq = getCheckpointFrequency();
    uint32_t lastQueued = 0;
    for (auto const& checkpoint : checkpointLedgers)
    {
        auto begin = firstLedgerInCheckpointContaining(checkpoint);
        populateCheckpointFilesFromDB(mApp, sess.session(), begin, freq,
                                      mCheckpointBuilder);
        LedgerHeaderUtils::copyToStream(db, sess.session(), begin, freq,
                                        mCheckpointBuilder);
        // Checkpoints in publish queue are complete, so we can finalize them
        mCheckpointBuilder.checkpointComplete(checkpoint);
        lastQueued = std::max(lastQueued, checkpoint);
    }

    auto lcl = LedgerHeaderUtils::loadMaxLedgerSeq(db);
    if (lastQueued < lcl)
    {
        // Then, reconstruct any partial checkpoints that haven't yet been
        // queued
        populateCheckpointFilesFromDB(mApp, sess.session(),
                                      firstLedgerInCheckpointContaining(lcl),
                                      freq, mCheckpointBuilder);
        LedgerHeaderUtils::copyToStream(db, sess.session(),
                                        firstLedgerInCheckpointContaining(lcl),
                                        freq, mCheckpointBuilder);
    }
    db.clearPreparedStatementCache();

    // Now it's safe to drop obsolete SQL tables
    sess.session() << "DROP TABLE IF EXISTS publishqueue;";
    dropSupportTxHistory(db);
    dropSupportTxSetHistory(db);
}

std::unique_ptr<HistoryManager>
HistoryManager::create(Application& app)
{
    return std::make_unique<HistoryManagerImpl>(app);
}

HistoryManagerImpl::HistoryManagerImpl(Application& app)
    : mApp(app)
    , mWorkDir(nullptr)
    , mPublishWork(nullptr)
    , mPublishSuccess(
          app.getMetrics().NewMeter({"history", "publish", "success"}, "event"))
    , mPublishFailure(
          app.getMetrics().NewMeter({"history", "publish", "failure"}, "event"))
    , mEnqueueToPublishTimer(
          app.getMetrics().NewTimer({"history", "publish", "time"}))
    , mCheckpointBuilder(app)
{
}

HistoryManagerImpl::~HistoryManagerImpl()
{
}

uint32_t
HistoryManagerImpl::getCheckpointFrequency() const
{
    if (mApp.getConfig().ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING)
    {
        return 8;
    }
    else
    {
        return 64;
    }
}

void
HistoryManagerImpl::logAndUpdatePublishStatus()
{
    std::stringstream stateStr;
    if (mPublishWork)
    {
        auto qlen = publishQueueLength();
        stateStr << "Publishing " << qlen << " queued checkpoints"
                 << " [" << getMinLedgerQueuedToPublish() << "-"
                 << getMaxLedgerQueuedToPublish() << "]"
                 << ": " << mPublishWork->getStatus();

        auto current = stateStr.str();
        auto existing = mApp.getStatusManager().getStatusMessage(
            StatusCategory::HISTORY_PUBLISH);
        if (existing != current)
        {
            CLOG_INFO(History, "{}", current);
            mApp.getStatusManager().setStatusMessage(
                StatusCategory::HISTORY_PUBLISH, current);
        }
    }
    else
    {
        mApp.getStatusManager().removeStatusMessage(
            StatusCategory::HISTORY_PUBLISH);
    }
}

bool
isPublishFile(std::string const& name)
{
    std::regex re("^[a-z0-9]{8}\\.json$");
    auto a = regex_match(name, re);
    return a;
}

bool
isPublishTmpFile(std::string const& name)
{
    std::regex re("^[a-z0-9]{8}\\.json.dirty$");
    auto a = regex_match(name, re);
    return a;
}

std::vector<std::string>
findPublishFiles(std::string const& dir)
{
    return fs::findfiles(dir, isPublishFile);
}

void
iterateOverCheckpoints(std::vector<std::string> const& files,
                       std::function<void(uint32_t, std::string const&)> f)
{
    for (auto const& file : files)
    {
        uint32_t seq = std::stoul(file.substr(0, 8), nullptr, 16);
        f(seq, file);
    }
}

void
forEveryQueuedCheckpoint(std::string const& dir,
                         std::function<void(uint32_t, std::string const&)> f)
{
    iterateOverCheckpoints(findPublishFiles(dir), f);
}

void
forEveryTmpCheckpoint(std::string const& dir,
                      std::function<void(uint32_t, std::string const&)> f)
{
    iterateOverCheckpoints(fs::findfiles(dir, isPublishTmpFile), f);
}

size_t
HistoryManagerImpl::publishQueueLength() const
{
    ZoneScoped;
    return findPublishFiles(publishQueuePath(mApp.getConfig()).string()).size();
}

string const&
HistoryManagerImpl::getTmpDir()
{
    ZoneScoped;
    if (!mWorkDir)
    {
        TmpDir t = mApp.getTmpDirManager().tmpDir("history");
        mWorkDir = std::make_unique<TmpDir>(std::move(t));
    }
    return mWorkDir->getName();
}

std::string
HistoryManagerImpl::localFilename(std::string const& basename)
{
    return this->getTmpDir() + "/" + basename;
}

uint32_t
HistoryManagerImpl::getMinLedgerQueuedToPublish()
{
    ZoneScoped;
    auto min = std::numeric_limits<uint32_t>::max();
    forEveryQueuedCheckpoint(
        publishQueuePath(mApp.getConfig()).string(),
        [&](uint32_t seq, std::string const& f) { min = std::min(min, seq); });
    return min;
}

uint32_t
HistoryManagerImpl::getMaxLedgerQueuedToPublish()
{
    ZoneScoped;
    auto max = std::numeric_limits<uint32_t>::min();
    forEveryQueuedCheckpoint(
        publishQueuePath(mApp.getConfig()).string(),
        [&](uint32_t seq, std::string const& f) { max = std::max(max, seq); });
    return max;
}

bool
HistoryManagerImpl::maybeQueueHistoryCheckpoint()
{
    uint32_t lcl = mApp.getLedgerManager().getLastClosedLedgerNum();
    if (!publishCheckpointOnLedgerClose(lcl))
    {
        return false;
    }

    if (!mApp.getHistoryArchiveManager().publishEnabled())
    {
        CLOG_DEBUG(History,
                   "Skipping checkpoint, no writable history archives");
        return false;
    }

    queueCurrentHistory();
    return true;
}

void
HistoryManagerImpl::queueCurrentHistory()
{
    ZoneScoped;
    auto ledger = mApp.getLedgerManager().getLastClosedLedgerNum();

    BucketList bl;
    if (mApp.getConfig().MODE_ENABLES_BUCKETLIST)
    {
        bl = mApp.getBucketManager().getBucketList();
    }

    HistoryArchiveState has(ledger, bl, mApp.getConfig().NETWORK_PASSPHRASE);

    CLOG_DEBUG(History, "Queueing publish state for ledger {}", ledger);
    mEnqueueTimes.emplace(ledger, std::chrono::steady_clock::now());

    // We queue history inside ledger commit, so do not finalize the file yet
    writeCheckpointFile(mApp, has, /* finalize */ false);

    // We have now written the current HAS to the database, so
    // it's "safe" to crash (at least after the enclosing tx commits);
    // but that HAS might have merges running and if we throw it
    // away at this point we'll lose the merges and have to restart
    // them. So instead we're going to insert the HAS we have in hand
    // into the in-memory publish queue in order to preserve those
    // merges-in-progress, avoid restarting them.

    mPublishQueued++;
    mPublishQueueBuckets.addBuckets(has.allBuckets());
}

void
HistoryManagerImpl::takeSnapshotAndPublish(HistoryArchiveState const& has)
{
    ZoneScoped;
    if (mPublishWork)
    {
        return;
    }
    // Ensure no merges are in-progress, and capture the bucket list hashes
    // *before* doing the actual publish. This ensures that the HAS is in
    // pristine state as returned by the database.
    for (auto const& bucket : has.currentBuckets)
    {
        releaseAssert(!bucket.next.isLive());
    }
    auto allBucketsFromHAS = has.allBuckets();
    auto ledgerSeq = has.currentLedger;
    CLOG_DEBUG(History, "Activating publish for ledger {}", ledgerSeq);
    auto snap = std::make_shared<StateSnapshot>(mApp, has);

    // Phase 1: resolve futures in snapshot
    auto resolveFutures = std::make_shared<ResolveSnapshotWork>(mApp, snap);
    // Phase 2: write snapshot files
    auto writeSnap = std::make_shared<WriteSnapshotWork>(mApp, snap);
    // Phase 3: update archives
    auto putSnap = std::make_shared<PutSnapshotFilesWork>(mApp, snap);

    std::vector<std::shared_ptr<BasicWork>> seq{resolveFutures, writeSnap,
                                                putSnap};

    auto start = mApp.getClock().now();
    ConditionFn delayTimeout = [start](Application& app) {
        auto delayDuration = app.getConfig().PUBLISH_TO_ARCHIVE_DELAY;

        auto time = app.getClock().now();
        auto dur = time - start;
        return std::chrono::duration_cast<std::chrono::seconds>(dur) >=
               delayDuration;
    };

    // Pass in all bucket hashes from HAS. We cannot rely on StateSnapshot
    // buckets here, because its buckets might have some futures resolved by
    // now, differing from the state of the bucketlist during queueing.
    //
    // NB: if WorkScheduler is aborting this returns nullptr, but that
    // which means we don't "really" start publishing.
    auto publishWork =
        std::make_shared<PublishWork>(mApp, snap, seq, allBucketsFromHAS);

    mPublishWork = mApp.getWorkScheduler().scheduleWork<ConditionalWork>(
        "delay-publishing-to-archive", delayTimeout, publishWork);
}

size_t
HistoryManagerImpl::publishQueuedHistory()
{
    if (mApp.isStopping())
    {
        return 0;
    }

#ifdef BUILD_TESTS
    if (!mPublicationEnabled)
    {
        CLOG_INFO(History,
                  "Publication explicitly disabled, so not publishing");
        return 0;
    }
#endif

    ZoneScoped;
    HistoryArchiveState has;
    auto seq = getMinLedgerQueuedToPublish();

    if (seq == std::numeric_limits<uint32_t>::max())
    {
        return 0;
    }

    auto file = publishQueuePath(mApp.getConfig()) / publishQueueFileName(seq);
    has.load(file.string());
    takeSnapshotAndPublish(has);
    return 1;
}

void
HistoryManagerImpl::maybeCheckpointComplete()
{
    uint32_t lcl = mApp.getLedgerManager().getLastClosedLedgerNum();
    if (!publishCheckpointOnLedgerClose(lcl) ||
        !mApp.getHistoryArchiveManager().publishEnabled())
    {
        return;
    }

    mCheckpointBuilder.checkpointComplete(lcl);

    auto finalizedHAS =
        publishQueuePath(mApp.getConfig()) / publishQueueFileName(lcl);
    if (fs::exists(finalizedHAS.string()))
    {
        CLOG_INFO(History, "{} exists, nothing to do", finalizedHAS);
        return;
    }

    auto temp = HistoryManager::publishQueuePath(mApp.getConfig()) /
                publishQueueTmpFileName(lcl);
    if (fs::exists(temp.string()) &&
        !fs::durableRename(
            temp.string(), finalizedHAS.string(),
            HistoryManager::publishQueuePath(mApp.getConfig()).string()))
    {
        throw std::runtime_error(fmt::format(
            "Failed to rename {} to {}", temp.string(), finalizedHAS.string()));
    }
}

std::vector<HistoryArchiveState>
HistoryManagerImpl::getPublishQueueStates()
{
    ZoneScoped;
    std::vector<HistoryArchiveState> states;
    forEveryQueuedCheckpoint(publishQueuePath(mApp.getConfig()).string(),
                             [&](uint32_t seq, std::string const& f) {
                                 HistoryArchiveState has;
                                 auto fullPath =
                                     publishQueuePath(mApp.getConfig()) / f;
                                 has.load(fullPath.string());
                                 states.push_back(has);
                             });
    return states;
}

PublishQueueBuckets::BucketCount
HistoryManagerImpl::loadBucketsReferencedByPublishQueue()
{
    ZoneScoped;
    auto states = getPublishQueueStates();
    PublishQueueBuckets::BucketCount result{};
    for (auto const& s : states)
    {
        auto sb = s.allBuckets();
        for (auto const& b : sb)
        {
            result[b]++;
        }
    }
    return result;
}

std::vector<std::string>
HistoryManagerImpl::getBucketsReferencedByPublishQueue()
{
    ZoneScoped;
    if (!mPublishQueueBucketsFilled)
    {
        mPublishQueueBuckets.setBuckets(loadBucketsReferencedByPublishQueue());
        mPublishQueueBucketsFilled = true;
    }

    std::vector<std::string> buckets;
    for (auto const& s : mPublishQueueBuckets.map())
    {
        buckets.push_back(s.first);
    }

    return buckets;
}

std::vector<std::string>
HistoryManagerImpl::getMissingBucketsReferencedByPublishQueue()
{
    ZoneScoped;
    auto states = getPublishQueueStates();
    std::set<std::string> buckets;
    for (auto const& s : states)
    {
        auto sb = mApp.getBucketManager().checkForMissingBucketsFiles(s);
        buckets.insert(sb.begin(), sb.end());
    }
    return std::vector<std::string>(buckets.begin(), buckets.end());
}

void
HistoryManagerImpl::deletePublishedFiles(uint32_t ledgerSeq, Config const& cfg)
{
    releaseAssert(isLastLedgerInCheckpoint(ledgerSeq));
    FileTransferInfo res(FileType::HISTORY_FILE_TYPE_RESULTS, ledgerSeq,
                         mApp.getConfig());
    FileTransferInfo txs(FileType::HISTORY_FILE_TYPE_TRANSACTIONS, ledgerSeq,
                         mApp.getConfig());
    FileTransferInfo headers(FileType::HISTORY_FILE_TYPE_LEDGER, ledgerSeq,
                             mApp.getConfig());
    // Dirty files shouldn't exist, but cleanup just in case
    std::remove(res.localPath_nogz_dirty().c_str());
    std::remove(txs.localPath_nogz_dirty().c_str());
    std::remove(headers.localPath_nogz_dirty().c_str());
    // Remove published files
    std::remove(res.localPath_nogz().c_str());
    std::remove(txs.localPath_nogz().c_str());
    std::remove(headers.localPath_nogz().c_str());
}

void
HistoryManagerImpl::historyPublished(
    uint32_t ledgerSeq, std::vector<std::string> const& originalBuckets,
    bool success)
{
    ZoneScoped;
    if (success)
    {
        auto iter = mEnqueueTimes.find(ledgerSeq);
        if (iter != mEnqueueTimes.end())
        {
            auto now = std::chrono::steady_clock::now();
            CLOG_DEBUG(
                Perf, "Published history for ledger {} in {} seconds",
                ledgerSeq,
                std::chrono::duration<double>(now - iter->second).count());
            mEnqueueToPublishTimer.Update(now - iter->second);
            mEnqueueTimes.erase(iter);
        }

        this->mPublishSuccess.Mark();
        auto file = publishQueuePath(mApp.getConfig()) /
                    publishQueueFileName(ledgerSeq);
        std::filesystem::remove(file);
        mPublishQueueBuckets.removeBuckets(originalBuckets);
        deletePublishedFiles(ledgerSeq, mApp.getConfig());
    }
    else
    {
        this->mPublishFailure.Mark();
    }
    mPublishWork.reset();
    mApp.postOnMainThread([this]() { this->publishQueuedHistory(); },
                          "HistoryManagerImpl: publishQueuedHistory");
}

void
HistoryManagerImpl::appendTransactionSet(uint32_t ledgerSeq,
                                         TxSetXDRFrameConstPtr const& txSet,
                                         TransactionResultSet const& resultSet)
{
    if (mApp.getHistoryArchiveManager().publishEnabled())
    {
        mCheckpointBuilder.appendTransactionSet(ledgerSeq, txSet, resultSet);
    }
}

void
HistoryManagerImpl::appendLedgerHeader(LedgerHeader const& header)
{
    if (mApp.getHistoryArchiveManager().publishEnabled())
    {
        mCheckpointBuilder.appendLedgerHeader(header);
#ifdef BUILD_TESTS
        if (header.ledgerSeq == mThrowOnAppend)
        {
            throw std::runtime_error("Throwing for testing");
        }
#endif
    }
}

void
HistoryManagerImpl::restoreCheckpoint(uint32_t lcl)
{
    if (mApp.getHistoryArchiveManager().publishEnabled())
    {
        mCheckpointBuilder.cleanup(lcl);
        // Remove any tmp checkpoints that were potentially queued _before_ a
        // crash, causing ledger rollback. These files will be finalized during
        // successful ledger close.
        forEveryTmpCheckpoint(publishQueuePath(mApp.getConfig()).string(),
                              [&](uint32_t seq, std::string const& f) {
                                  if (seq > lcl)
                                  {
                                      std::remove(f.c_str());
                                  }
                              });
        // Maybe finalize checkpoint if we're at a checkpoint boundary and
        // haven't rotated yet. No-op if checkpoint has been rotated already
        maybeCheckpointComplete();
    }
}

uint64_t
HistoryManagerImpl::getPublishQueueCount() const
{
    return mPublishQueued;
}

uint64_t
HistoryManagerImpl::getPublishSuccessCount() const
{
    return mPublishSuccess.count();
}

uint64_t
HistoryManagerImpl::getPublishFailureCount() const
{
    return mPublishFailure.count();
}

#ifdef BUILD_TESTS
void
HistoryManagerImpl::setPublicationEnabled(bool enabled)
{
    CLOG_INFO(History, "{} history publication",
              (enabled ? "Enabling" : "Disabling"));
    mPublicationEnabled = enabled;
}
#endif
}
