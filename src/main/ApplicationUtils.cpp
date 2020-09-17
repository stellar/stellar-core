// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/ApplicationUtils.h"
#include "bucket/Bucket.h"
#include "bucket/BucketManager.h"
#include "catchup/ApplyBucketsWork.h"
#include "catchup/CatchupConfiguration.h"
#include "database/Database.h"
#include "history/HistoryArchive.h"
#include "history/HistoryArchiveManager.h"
#include "historywork/GetHistoryArchiveStateWork.h"
#include "ledger/LedgerManager.h"
#include "main/ErrorMessages.h"
#include "main/ExternalQueue.h"
#include "main/Maintainer.h"
#include "main/PersistentState.h"
#include "main/StellarCoreVersion.h"
#include "overlay/OverlayManager.h"
#include "util/Logging.h"
#include "work/WorkScheduler.h"

#include <lib/http/HttpClient.h>
#include <locale>

namespace stellar
{

int
runWithConfig(Config cfg, optional<CatchupConfiguration> cc)
{
    VirtualClock::Mode clockMode = VirtualClock::REAL_TIME;

    if (cfg.MANUAL_CLOSE)
    {
        if (!cfg.NODE_IS_VALIDATOR)
        {
            LOG(ERROR) << "Starting stellar-core in MANUAL_CLOSE mode requires "
                          "NODE_IS_VALIDATOR to be set";
            return 1;
        }
        if (cfg.RUN_STANDALONE)
        {
            clockMode = VirtualClock::VIRTUAL_TIME;
            if (cfg.AUTOMATIC_MAINTENANCE_COUNT != 0 ||
                cfg.AUTOMATIC_MAINTENANCE_PERIOD.count() != 0)
            {
                LOG(WARNING)
                    << "Using MANUAL_CLOSE and RUN_STANDALONE together "
                       "induces virtual time, which requires automatic "
                       "maintenance to be disabled.  "
                       "AUTOMATIC_MAINTENANCE_COUNT and "
                       "AUTOMATIC_MAINTENANCE_PERIOD are being overridden to "
                       "0.";
                cfg.AUTOMATIC_MAINTENANCE_COUNT = 0;
                cfg.AUTOMATIC_MAINTENANCE_PERIOD = std::chrono::seconds{0};
            }
        }
    }

    LOG(INFO) << "Starting stellar-core " << STELLAR_CORE_VERSION;
    VirtualClock clock(clockMode);
    Application::pointer app;
    try
    {
        app = Application::create(clock, cfg, false);

        if (!app->getHistoryArchiveManager().checkSensibleConfig())
        {
            return 1;
        }
        if (cfg.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING)
        {
            LOG(WARNING) << "Artificial acceleration of time enabled "
                         << "(for testing only)";
        }

        if (cc)
        {
            Json::Value catchupInfo;
            std::shared_ptr<HistoryArchive> archive;
            auto const& ham = app->getHistoryArchiveManager();
            archive = ham.selectRandomReadableHistoryArchive();
            int res = catchup(app, *cc, catchupInfo, archive);
            if (res != 0)
            {
                return res;
            }
        }
        else
        {
            app->start();
        }

        if (!cfg.MODE_AUTO_STARTS_OVERLAY)
        {
            app->getOverlayManager().start();
        }

        app->applyCfgCommands();
    }
    catch (std::exception const& e)
    {
        LOG(FATAL) << "Got an exception: " << e.what();
        LOG(FATAL) << REPORT_INTERNAL_BUG;
        return 1;
    }

    try
    {
        auto& io = clock.getIOContext();
        asio::io_context::work mainWork(io);
        while (!io.stopped())
        {
            clock.crank();
        }
    }
    catch (std::exception const& e)
    {
        LOG(FATAL) << "Got an exception: " << e.what();
        throw; // propagate exception (core dump, etc)
    }
    return 0;
}

void
httpCommand(std::string const& command, unsigned short port)
{
    std::string ret;
    std::ostringstream path;

    path << "/";
    bool gotCommand = false;

    std::locale loc("C");

    for (auto const& c : command)
    {
        if (gotCommand)
        {
            if (std::isalnum(c, loc))
            {
                path << c;
            }
            else
            {
                path << '%' << std::hex << std::setw(2) << std::setfill('0')
                     << (unsigned int)c;
            }
        }
        else
        {
            path << c;
            if (c == '?')
            {
                gotCommand = true;
            }
        }
    }

    int code = http_request("127.0.0.1", path.str(), port, ret);
    if (code == 200)
    {
        LOG(INFO) << ret;
    }
    else
    {
        LOG(INFO) << "http failed(" << code << ") port: " << port
                  << " command: " << command;
    }
}

void
setForceSCPFlag()
{
    LOG(WARNING) << "* ";
    LOG(WARNING) << "* Nothing to do: `force scp` command has been deprecated";
    LOG(WARNING) << "* Refer to `--wait-for-consensus` run option instead";
    LOG(WARNING) << "* ";
}

void
initializeDatabase(Config cfg)
{
    VirtualClock clock;
    cfg.setNoListen();
    Application::pointer app = Application::create(clock, cfg);

    LOG(INFO) << "*";
    LOG(INFO) << "* The next launch will catchup from the network afresh.";
    LOG(INFO) << "*";
}

void
showOfflineInfo(Config cfg)
{
    // needs real time to display proper stats
    VirtualClock clock(VirtualClock::REAL_TIME);
    cfg.setNoListen();
    Application::pointer app = Application::create(clock, cfg, false);
    app->reportInfo();
}

#ifdef BUILD_TESTS
void
loadXdr(Config cfg, std::string const& bucketFile)
{
    VirtualClock clock;
    cfg.setNoListen();
    Application::pointer app = Application::create(clock, cfg, false);

    uint256 zero;
    Bucket bucket(bucketFile, zero);
    bucket.apply(*app);
}

int
rebuildLedgerFromBuckets(Config cfg)
{
    VirtualClock clock(VirtualClock::REAL_TIME);
    cfg.setNoListen();
    Application::pointer app = Application::create(clock, cfg, false);

    auto& ps = app->getPersistentState();
    auto lcl = ps.getState(PersistentState::kLastClosedLedger);
    auto hasStr = ps.getState(PersistentState::kHistoryArchiveState);
    auto pass = ps.getState(PersistentState::kNetworkPassphrase);

    LOG(INFO) << "Re-initializing database ledger tables.";
    auto& db = app->getDatabase();
    auto& session = app->getDatabase().getSession();
    soci::transaction tx(session);

    db.initialize();
    db.upgradeToCurrentSchema();
    db.clearPreparedStatementCache();
    LOG(INFO) << "Re-initialized database ledger tables.";

    LOG(INFO) << "Re-storing persistent state.";
    ps.setState(PersistentState::kLastClosedLedger, lcl);
    ps.setState(PersistentState::kHistoryArchiveState, hasStr);
    ps.setState(PersistentState::kNetworkPassphrase, pass);

    LOG(INFO) << "Applying buckets from LCL bucket list.";
    std::map<std::string, std::shared_ptr<Bucket>> localBuckets;
    auto& ws = app->getWorkScheduler();

    HistoryArchiveState has;
    has.fromString(hasStr);
    has.prepareForPublish(*app);

    auto applyBucketsWork = ws.executeWork<ApplyBucketsWork>(
        localBuckets, has, Config::CURRENT_LEDGER_PROTOCOL_VERSION);
    auto ok = applyBucketsWork->getState() == BasicWork::State::WORK_SUCCESS;
    if (ok)
    {
        tx.commit();
        LOG(INFO) << "*";
        LOG(INFO) << "* Rebuilt ledger from buckets successfully.";
        LOG(INFO) << "*";
    }
    else
    {
        tx.rollback();
        LOG(INFO) << "*";
        LOG(INFO) << "* Rebuild of ledger failed.";
        LOG(INFO) << "*";
    }

    app->gracefulStop();
    while (clock.crank(true))
        ;
    return ok ? 0 : 1;
}
#endif

int
reportLastHistoryCheckpoint(Config cfg, std::string const& outputFile)
{
    VirtualClock clock(VirtualClock::REAL_TIME);
    cfg.setNoListen();
    Application::pointer app = Application::create(clock, cfg, false);

    auto& wm = app->getWorkScheduler();
    auto getHistoryArchiveStateWork =
        wm.executeWork<GetHistoryArchiveStateWork>();

    auto ok = getHistoryArchiveStateWork->getState() ==
              BasicWork::State::WORK_SUCCESS;
    if (ok)
    {
        auto state = getHistoryArchiveStateWork->getHistoryArchiveState();
        std::string filename = outputFile.empty() ? "-" : outputFile;

        if (filename == "-")
        {
            LOG(INFO) << "*";
            LOG(INFO) << "* Last history checkpoint " << state.toString();
            LOG(INFO) << "*";
        }
        else
        {
            state.save(filename);
            LOG(INFO) << "*";
            LOG(INFO) << "* Wrote last history checkpoint " << filename;
            LOG(INFO) << "*";
        }
    }
    else
    {
        LOG(INFO) << "*";
        LOG(INFO) << "* Fetching last history checkpoint failed.";
        LOG(INFO) << "*";
    }

    app->gracefulStop();
    while (clock.crank(true))
        ;

    return ok ? 0 : 1;
}

void
genSeed()
{
    auto key = SecretKey::random();
    std::cout << "Secret seed: " << key.getStrKeySeed().value << std::endl;
    std::cout << "Public: " << key.getStrKeyPublic() << std::endl;
}

int
initializeHistories(Config cfg, std::vector<std::string> const& newHistories)
{
    VirtualClock clock;
    cfg.setNoListen();
    Application::pointer app = Application::create(clock, cfg, false);

    for (auto const& arch : newHistories)
    {
        if (!app->getHistoryArchiveManager().initializeHistoryArchive(arch))
            return 1;
    }
    return 0;
}

void
writeCatchupInfo(Json::Value const& catchupInfo, std::string const& outputFile)
{
    std::string filename = outputFile.empty() ? "-" : outputFile;
    auto content = catchupInfo.toStyledString();

    if (filename == "-")
    {
        LOG(INFO) << "*";
        LOG(INFO) << "* Catchup info: " << content;
        LOG(INFO) << "*";
    }
    else
    {
        std::ofstream out{};
        out.exceptions(std::ios::failbit | std::ios::badbit);
        out.open(filename);
        out.write(content.c_str(), content.size());
        out.close();

        LOG(INFO) << "*";
        LOG(INFO) << "* Wrote catchup info to " << filename;
        LOG(INFO) << "*";
    }
}

int
catchup(Application::pointer app, CatchupConfiguration cc,
        Json::Value& catchupInfo, std::shared_ptr<HistoryArchive> archive)
{
    app->start();

    try
    {
        app->getLedgerManager().startCatchup(cc, archive);
    }
    catch (std::invalid_argument const&)
    {
        LOG(INFO) << "*";
        LOG(INFO) << "* Target ledger " << cc.toLedger()
                  << " is not newer than last closed ledger "
                  << app->getLedgerManager().getLastClosedLedgerNum()
                  << " - nothing to do";
        LOG(INFO) << "* If you really want to catchup to " << cc.toLedger()
                  << " run stellar-core new-db";
        LOG(INFO) << "*";
        return 2;
    }

    auto& clock = app->getClock();
    auto& io = clock.getIOContext();
    auto synced = false;
    asio::io_context::work mainWork(io);
    auto done = false;
    while (!done && clock.crank(true))
    {
        switch (app->getCatchupManager().getCatchupWorkState())
        {
        case BasicWork::State::WORK_ABORTED:
        case BasicWork::State::WORK_FAILURE:
        {
            done = true;
            break;
        }
        case BasicWork::State::WORK_SUCCESS:
        {
            done = true;
            synced = true;
            break;
        }
        case BasicWork::State::WORK_RUNNING:
        case BasicWork::State::WORK_WAITING:
        {
            break;
        }
        default:
            abort();
        }
    }

    LOG(INFO) << "*";
    if (synced)
    {
        LOG(INFO) << "* Catchup finished.";
    }
    else
    {
        LOG(INFO) << "* Catchup failed.";
    }
    LOG(INFO) << "*";

    catchupInfo = app->getJsonInfo();
    return synced ? 0 : 3;
}

int
publish(Application::pointer app)
{
    app->start();

    auto& clock = app->getClock();
    auto& io = clock.getIOContext();
    asio::io_context::work mainWork(io);

    auto lcl = app->getLedgerManager().getLastClosedLedgerNum();
    auto isCheckpoint = app->getHistoryManager().isLastLedgerInCheckpoint(lcl);
    auto expectedPublishQueueSize = isCheckpoint ? 1 : 0;

    app->getHistoryManager().publishQueuedHistory();
    while (app->getHistoryManager().publishQueueLength() !=
               expectedPublishQueueSize &&
           clock.crank(true))
    {
    }

    // Cleanup buckets not referenced by publish queue anymore
    app->getBucketManager().forgetUnreferencedBuckets();

    LOG(INFO) << "*";
    LOG(INFO) << "* Publish finished.";
    LOG(INFO) << "*";

    return 0;
}
}
