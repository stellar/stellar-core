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
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "main/Config.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "overlay/StellarXDR.h"
#include "process/ProcessManager.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/StatusManager.h"
#include "util/TmpDir.h"
#include "work/WorkScheduler.h"
#include "xdrpp/marshal.h"
#include <Tracy.hpp>
#include <fmt/format.h>

#include <fstream>
#include <system_error>

namespace stellar
{

using namespace std;

static string kSQLCreateStatement = "CREATE TABLE IF NOT EXISTS publishqueue ("
                                    "ledger   INTEGER PRIMARY KEY,"
                                    "state    TEXT"
                                    "); ";

void
HistoryManager::dropAll(Database& db)
{
    db.getSession() << "DROP TABLE IF EXISTS publishqueue;";
    soci::statement st = db.getSession().prepare << kSQLCreateStatement;
    st.execute(true);
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

size_t
HistoryManagerImpl::publishQueueLength() const
{
    ZoneScoped;
    uint32_t count;
    auto prep = mApp.getDatabase().getPreparedStatement(
        "SELECT count(ledger) FROM publishqueue;");
    auto& st = prep.statement();
    st.exchange(soci::into(count));
    st.define_and_bind();
    st.execute(true);
    return count;
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

InferredQuorum
HistoryManagerImpl::inferQuorum(uint32_t ledgerNum)
{
    InferredQuorum iq;
    CLOG_INFO(History, "Starting FetchRecentQsetsWork");
    mApp.getWorkScheduler().executeWork<FetchRecentQsetsWork>(iq, ledgerNum);
    return iq;
}

uint32_t
HistoryManagerImpl::getMinLedgerQueuedToPublish()
{
    ZoneScoped;
    uint32_t seq;
    soci::indicator minIndicator;
    auto prep = mApp.getDatabase().getPreparedStatement(
        "SELECT min(ledger) FROM publishqueue;");
    auto& st = prep.statement();
    st.exchange(soci::into(seq, minIndicator));
    st.define_and_bind();
    st.execute(true);
    if (minIndicator == soci::indicator::i_ok)
    {
        return seq;
    }
    return 0;
}

uint32_t
HistoryManagerImpl::getMaxLedgerQueuedToPublish()
{
    ZoneScoped;
    uint32_t seq;
    soci::indicator maxIndicator;
    auto prep = mApp.getDatabase().getPreparedStatement(
        "SELECT max(ledger) FROM publishqueue;");
    auto& st = prep.statement();
    st.exchange(soci::into(seq, maxIndicator));
    st.define_and_bind();
    st.execute(true);
    if (maxIndicator == soci::indicator::i_ok)
    {
        return seq;
    }
    return 0;
}

bool
HistoryManagerImpl::maybeQueueHistoryCheckpoint()
{
    uint32_t lcl = mApp.getLedgerManager().getLastClosedLedgerNum();
    if (!publishCheckpointOnLedgerClose(lcl))
    {
        return false;
    }

    if (!mApp.getHistoryArchiveManager().hasAnyWritableHistoryArchive())
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
    HistoryArchiveState has(ledger, mApp.getBucketManager().getBucketList(),
                            mApp.getConfig().NETWORK_PASSPHRASE);

    CLOG_DEBUG(History, "Queueing publish state for ledger {}", ledger);
    mEnqueueTimes.emplace(ledger, std::chrono::steady_clock::now());

    auto state = has.toString();
    auto timer = mApp.getDatabase().getInsertTimer("publishqueue");
    auto prep = mApp.getDatabase().getPreparedStatement(
        "INSERT INTO publishqueue (ledger, state) VALUES (:lg, :st);");
    auto& st = prep.statement();
    st.exchange(soci::use(ledger));
    st.exchange(soci::use(state));
    st.define_and_bind();
    st.execute(true);

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
        assert(!bucket.next.isLive());
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
    // Pass in all bucket hashes from HAS. We cannot rely on StateSnapshot
    // buckets here, because its buckets might have some futures resolved by
    // now, differing from the state of the bucketlist during queueing.
    //
    // NB: if WorkScheduler is aborting this returns nullptr, but that
    // which means we don't "really" start publishing.
    mPublishWork = mApp.getWorkScheduler().scheduleWork<PublishWork>(
        snap, seq, allBucketsFromHAS);
}

size_t
HistoryManagerImpl::publishQueuedHistory()
{
#ifdef BUILD_TESTS
    if (!mPublicationEnabled)
    {
        CLOG_INFO(History,
                  "Publication explicitly disabled, so not publishing");
        return 0;
    }
#endif

    ZoneScoped;
    std::string state;

    auto prep = mApp.getDatabase().getPreparedStatement(
        "SELECT state FROM publishqueue"
        " ORDER BY ledger ASC LIMIT 1;");
    auto& st = prep.statement();
    soci::indicator stateIndicator;
    st.exchange(soci::into(state, stateIndicator));
    st.define_and_bind();
    st.execute(true);
    if (st.got_data() && stateIndicator == soci::indicator::i_ok)
    {
        HistoryArchiveState has;
        has.fromString(state);
        takeSnapshotAndPublish(has);
        return 1;
    }
    return 0;
}

std::vector<HistoryArchiveState>
HistoryManagerImpl::getPublishQueueStates()
{
    ZoneScoped;
    std::vector<HistoryArchiveState> states;

    std::string state;
    auto prep = mApp.getDatabase().getPreparedStatement(
        "SELECT state FROM publishqueue;");
    auto& st = prep.statement();
    st.exchange(soci::into(state));
    st.define_and_bind();
    st.execute(true);
    while (st.got_data())
    {
        states.emplace_back();
        states.back().fromString(state);
        st.fetch();
    }
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
        auto timer = mApp.getDatabase().getDeleteTimer("publishqueue");
        auto prep = mApp.getDatabase().getPreparedStatement(
            "DELETE FROM publishqueue WHERE ledger = :lg;");
        auto& st = prep.statement();
        st.exchange(soci::use(ledgerSeq));
        st.define_and_bind();
        st.execute(true);

        mPublishQueueBuckets.removeBuckets(originalBuckets);
    }
    else
    {
        this->mPublishFailure.Mark();
    }
    mPublishWork.reset();
    mApp.postOnMainThread([this]() { this->publishQueuedHistory(); },
                          "HistoryManagerImpl: publishQueuedHistory");
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
