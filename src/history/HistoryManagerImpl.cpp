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
#include "historywork/RepairMissingBucketsWork.h"
#include "ledger/LedgerManager.h"
#include "lib/util/format.h"
#include "main/Application.h"
#include "main/Config.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "overlay/StellarXDR.h"
#include "process/ProcessManager.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/StatusManager.h"
#include "util/TmpDir.h"
#include "work/WorkManager.h"
#include "xdrpp/marshal.h"

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

uint32_t
HistoryManagerImpl::checkpointContainingLedger(uint32_t ledger) const
{
    return nextCheckpointLedger(ledger + 1) - 1;
}

uint32_t
HistoryManagerImpl::prevCheckpointLedger(uint32_t ledger) const
{
    uint32_t freq = getCheckpointFrequency();
    return (ledger / freq) * freq;
}

uint32_t
HistoryManagerImpl::nextCheckpointLedger(uint32_t ledger) const
{
    uint32_t freq = getCheckpointFrequency();
    if (ledger == 0)
        return freq;
    return (((ledger + freq - 1) / freq) * freq);
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
            CLOG(INFO, "History") << current;
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

HistoryArchiveState
HistoryManagerImpl::getLastClosedHistoryArchiveState() const
{
    auto seq = mApp.getLedgerManager().getLastClosedLedgerNum();
    auto& bl = mApp.getBucketManager().getBucketList();
    return HistoryArchiveState(seq, bl);
}

InferredQuorum
HistoryManagerImpl::inferQuorum()
{
    InferredQuorum iq;
    CLOG(INFO, "History") << "Starting FetchRecentQsetsWork";
    mApp.getWorkManager().executeWork<FetchRecentQsetsWork>(iq);
    return iq;
}

uint32_t
HistoryManagerImpl::getMinLedgerQueuedToPublish()
{
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
    uint32_t seq = mApp.getLedgerManager().getLastClosedLedgerNum() + 1;
    if (seq != nextCheckpointLedger(seq))
    {
        return false;
    }

    if (!mApp.getHistoryArchiveManager().hasAnyWritableHistoryArchive())
    {
        CLOG(DEBUG, "History")
            << "Skipping checkpoint, no writable history archives";
        return false;
    }

    queueCurrentHistory();
    return true;
}

void
HistoryManagerImpl::queueCurrentHistory()
{
    auto has = getLastClosedHistoryArchiveState();

    auto ledger = has.currentLedger;
    CLOG(DEBUG, "History") << "Queueing publish state for ledger " << ledger;
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
    if (mPublishWork)
    {
        return;
    }
    auto ledgerSeq = has.currentLedger;
    CLOG(DEBUG, "History") << "Activating publish for ledger " << ledgerSeq;
    auto snap = std::make_shared<StateSnapshot>(mApp, has);

    mPublishWork = mApp.getWorkManager().addWork<PublishWork>(snap);
    mApp.getWorkManager().advanceChildren();
}

size_t
HistoryManagerImpl::publishQueuedHistory()
{
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
    if (success)
    {
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

void
HistoryManagerImpl::downloadMissingBuckets(
    HistoryArchiveState desiredState,
    std::function<void(asio::error_code const& ec)> handler)
{
    CLOG(INFO, "History") << "Starting RepairMissingBucketsWork";
    mApp.getWorkManager().addWork<RepairMissingBucketsWork>(desiredState,
                                                            handler);
    mApp.getWorkManager().advanceChildren();
}

uint64_t
HistoryManagerImpl::getPublishQueueCount()
{
    return mPublishQueued;
}

uint64_t
HistoryManagerImpl::getPublishSuccessCount()
{
    return mPublishSuccess.count();
}

uint64_t
HistoryManagerImpl::getPublishFailureCount()
{
    return mPublishFailure.count();
}
}
