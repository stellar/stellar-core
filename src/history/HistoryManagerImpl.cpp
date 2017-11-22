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
#include "history/HistoryManagerImpl.h"
#include "history/StateSnapshot.h"
#include "historywork/FetchRecentQsetsWork.h"
#include "historywork/GetHistoryArchiveStateWork.h"
#include "historywork/PublishWork.h"
#include "historywork/PutHistoryArchiveStateWork.h"
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
#include "util/make_unique.h"
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

bool
HistoryManager::initializeHistoryArchive(Application& app, std::string arch)
{
    auto const& cfg = app.getConfig();
    auto i = cfg.HISTORY.find(arch);
    if (i == cfg.HISTORY.end())
    {
        CLOG(FATAL, "History")
            << "Can't initialize unknown history archive '" << arch << "'";
        return false;
    }

    auto& wm = app.getWorkManager();

    // First check that there's no existing HAS in the archive
    HistoryArchiveState existing;
    CLOG(INFO, "History") << "Probing history archive '" << arch
                          << "' for existing state";
    auto getHas = wm.executeWork<GetHistoryArchiveStateWork>(
        false, "get-history-archive-state", existing, 0,
        std::chrono::seconds(0), i->second, 0);
    if (getHas->getState() == Work::WORK_SUCCESS)
    {
        CLOG(ERROR, "History")
            << "History archive '" << arch << "' already initialized!";
        return false;
    }
    CLOG(INFO, "History") << "History archive '" << arch
                          << "' appears uninitialized";

    HistoryArchiveState has;
    CLOG(INFO, "History") << "Initializing history archive '" << arch << "'";
    has.resolveAllFutures();

    auto putHas =
        wm.executeWork<PutHistoryArchiveStateWork>(false, has, i->second);
    if (putHas->getState() == Work::WORK_SUCCESS)
    {
        CLOG(INFO, "History") << "Initialized history archive '" << arch << "'";
        return true;
    }
    else
    {
        CLOG(FATAL, "History")
            << "Failed to initialize history archive '" << arch << "'";
        return false;
    }
}

std::unique_ptr<HistoryManager>
HistoryManager::create(Application& app)
{
    return make_unique<HistoryManagerImpl>(app);
}

bool
HistoryManager::checkSensibleConfig(Config const& cfg)
{
    // Check reasonable-ness of history archive definitions
    std::vector<std::string> readOnlyArchives;
    std::vector<std::string> readWriteArchives;
    std::vector<std::string> writeOnlyArchives;
    std::vector<std::string> inertArchives;

    for (auto const& pair : cfg.HISTORY)
    {
        if (pair.second->hasGetCmd())
        {
            if (pair.second->hasPutCmd())
            {
                readWriteArchives.push_back(pair.first);
            }
            else
            {
                readOnlyArchives.push_back(pair.first);
            }
        }
        else
        {
            if (pair.second->hasPutCmd())
            {
                writeOnlyArchives.push_back(pair.first);
            }
            else
            {
                inertArchives.push_back(pair.first);
            }
        }
    }

    bool badArchives = false;

    for (auto const& a : inertArchives)
    {
        CLOG(FATAL, "History")
            << "Archive '" << a
            << "' has no 'get' or 'put' command, will not function";
        badArchives = true;
    }

    for (auto const& a : writeOnlyArchives)
    {
        CLOG(FATAL, "History")
            << "Archive '" << a
            << "' has 'put' but no 'get' command, will be unwritable";
        badArchives = true;
    }

    for (auto const& a : readWriteArchives)
    {
        CLOG(INFO, "History")
            << "Archive '" << a
            << "' has 'put' and 'get' commands, will be read and written";
    }

    for (auto const& a : readOnlyArchives)
    {
        CLOG(INFO, "History")
            << "Archive '" << a
            << "' has 'get' command only, will not be written";
    }

    if (readOnlyArchives.empty() && readWriteArchives.empty())
    {
        CLOG(FATAL, "History")
            << "No readable archives configured, catchup will fail.";
        badArchives = true;
    }

    if (readWriteArchives.empty())
    {
        CLOG(WARNING, "History")
            << "No writable archives configured, history will not be written.";
    }

    if (badArchives)
    {
        CLOG(ERROR, "History") << "History archives misconfigured.";
        return false;
    }
    return true;
}

HistoryManagerImpl::HistoryManagerImpl(Application& app)
    : mApp(app)
    , mWorkDir(nullptr)
    , mPublishWork(nullptr)

    , mPublishSkip(
          app.getMetrics().NewMeter({"history", "publish", "skip"}, "event"))
    , mPublishQueue(
          app.getMetrics().NewMeter({"history", "publish", "queue"}, "event"))
    , mPublishDelay(
          app.getMetrics().NewMeter({"history", "publish", "delay"}, "event"))
    , mPublishStart(
          app.getMetrics().NewMeter({"history", "publish", "start"}, "event"))
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

uint64_t
HistoryManagerImpl::nextCheckpointCatchupProbe(uint32_t ledger) const
{
    uint32_t next = this->nextCheckpointLedger(ledger);

    auto ledger_duration =
        Herder::EXP_LEDGER_TIMESPAN_SECONDS + std::chrono::seconds(1);

    if (mApp.getConfig().ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING)
    {
        ledger_duration = std::chrono::seconds(1);
    }

    return (((next - ledger) + 5) * ledger_duration.count());
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
        mWorkDir = make_unique<TmpDir>(std::move(t));
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
    auto seq =
        mApp.getLedgerManager().getLastClosedLedgerHeader().header.ledgerSeq;
    auto& bl = mApp.getBucketManager().getBucketList();
    return HistoryArchiveState(seq, bl);
}

InferredQuorum
HistoryManagerImpl::inferQuorum()
{
    InferredQuorum iq;
    bool done = false;
    auto handler = [&done](asio::error_code const& ec) { done = true; };
    CLOG(INFO, "History") << "Starting FetchRecentQsetsWork";
    mApp.getWorkManager().addWork<FetchRecentQsetsWork>(iq, handler);
    mApp.getWorkManager().advanceChildren();
    while (!done)
    {
        mApp.getClock().crank(false);
    }
    return iq;
}

bool
HistoryManagerImpl::hasAnyWritableHistoryArchive()
{
    auto const& hist = mApp.getConfig().HISTORY;
    for (auto const& pair : hist)
    {
        if (pair.second->hasGetCmd() && pair.second->hasPutCmd())
            return true;
    }
    return false;
}

std::shared_ptr<HistoryArchive>
HistoryManagerImpl::selectRandomReadableHistoryArchive()
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
        throw std::runtime_error("No GET-enabled history archive in config");
    }
    else if (archives.size() == 1)
    {
        CLOG(DEBUG, "History")
            << "Fetching from sole readable history archive '"
            << archives[0].first << "'";
        return archives[0].second;
    }
    else
    {
        std::uniform_int_distribution<size_t> dist(0, archives.size() - 1);
        size_t i = dist(gRandomEngine);
        CLOG(DEBUG, "History") << "Fetching from readable history archive #"
                               << i << ", '" << archives[i].first << "'";
        return archives[i].second;
    }
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
    uint32_t seq = mApp.getLedgerManager().getLedgerNum();
    if (seq != nextCheckpointLedger(seq))
    {
        return false;
    }

    if (!hasAnyWritableHistoryArchive())
    {
        mPublishSkip.Mark();
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

    mPublishQueue.Mark();
    mPublishQueueBuckets.addBuckets(has.allBuckets());
    takeSnapshotAndPublish(has);
}

void
HistoryManagerImpl::takeSnapshotAndPublish(HistoryArchiveState const& has)
{
    if (mPublishWork)
    {
        mPublishDelay.Mark();
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

std::vector<std::string>
HistoryManagerImpl::loadBucketsReferencedByPublishQueue()
{
    auto states = getPublishQueueStates();
    std::set<std::string> buckets;
    for (auto const& s : states)
    {
        auto sb = s.allBuckets();
        buckets.insert(sb.begin(), sb.end());
    }
    return std::vector<std::string>(buckets.begin(), buckets.end());
}

std::vector<std::string>
HistoryManagerImpl::getBucketsReferencedByPublishQueue()
{
    if (!mPublishQueueBucketsFilled)
    {
        mPublishQueueBuckets.addBuckets(loadBucketsReferencedByPublishQueue());
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
    mApp.getClock().getIOService().post(
        [this]() { this->publishQueuedHistory(); });
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
HistoryManagerImpl::getPublishSkipCount()
{
    return mPublishSkip.count();
}

uint64_t
HistoryManagerImpl::getPublishQueueCount()
{
    return mPublishQueue.count();
}

uint64_t
HistoryManagerImpl::getPublishDelayCount()
{
    return mPublishDelay.count();
}

uint64_t
HistoryManagerImpl::getPublishStartCount()
{
    return mPublishStart.count();
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
