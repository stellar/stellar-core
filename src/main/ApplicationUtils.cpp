// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/ApplicationUtils.h"
#include "bucket/Bucket.h"
#include "bucket/BucketManager.h"
#include "catchup/ApplyBucketsWork.h"
#include "catchup/CatchupConfiguration.h"
#include "crypto/Hex.h"
#include "database/Database.h"
#include "herder/Herder.h"
#include "history/HistoryArchive.h"
#include "history/HistoryArchiveManager.h"
#include "history/HistoryArchiveReportWork.h"
#include "historywork/GetHistoryArchiveStateWork.h"
#include "invariant/BucketListIsConsistentWithDatabase.h"
#include "ledger/LedgerHeaderUtils.h"
#include "ledger/LedgerManager.h"
#include "main/ErrorMessages.h"
#include "main/ExternalQueue.h"
#include "main/Maintainer.h"
#include "main/PersistentState.h"
#include "main/StellarCoreVersion.h"
#include "overlay/OverlayManager.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/XDRCereal.h"
#include "util/xdrquery/XDRQuery.h"
#include "work/WorkScheduler.h"

#include <filesystem>
#include <lib/http/HttpClient.h>
#include <locale>
#include <map>
#include <optional>

namespace stellar
{
namespace
{
void
writeLedgerAggregationTable(
    std::ofstream& ofs,
    std::optional<xdrquery::XDRFieldExtractor> const& groupByExtractor,
    std::map<std::vector<xdrquery::ResultType>, xdrquery::XDRAccumulator> const&
        accumulators)
{
    std::vector<std::string> keyFields;
    if (groupByExtractor)
    {
        keyFields = groupByExtractor->getFieldNames();
        for (auto const& keyField : keyFields)
        {
            ofs << keyField << ",";
        }
    }
    if (!accumulators.empty())
    {
        auto const& [_, accumulator] = *accumulators.begin();
        for (auto const& acc : accumulator.getAccumulators())
        {
            ofs << acc->getName() << ",";
        }
    }
    ofs << std::endl;

    for (auto const& [key, accumulator] : accumulators)
    {
        if (!key.empty())
        {
            for (size_t i = 0; i < key.size(); ++i)
            {
                if (key[i])
                {
                    ofs << xdrquery::resultToString(*key[i]);
                }
                ofs << ",";
            }
        }
        for (auto const& acc : accumulator.getAccumulators())
        {
            ofs << std::visit([](auto&& v) { return fmt::to_string(v); },
                              acc->getValue())
                << ",";
        }
        ofs << std::endl;
    }
}
} // namespace

const std::string MINIMAL_DB_NAME = "minimal.db";

bool
canRebuildInMemoryLedgerFromBuckets(uint32_t startAtLedger, uint32_t lcl)
{
    // Number of streaming ledgers ahead of LCL. Core will
    // rebuild the existing state if the difference between the start
    // ledger and LCL is within this window.
    uint32_t const RESTORE_STATE_LEDGER_WINDOW = 10;
    // Do not rebuild genesis state
    bool isGenesis = lcl == LedgerManager::GENESIS_LEDGER_SEQ;
    return !isGenesis && startAtLedger >= lcl &&
           startAtLedger - lcl <= RESTORE_STATE_LEDGER_WINDOW;
}

std::filesystem::path
minimalDbPath(Config const& cfg)
{
    std::filesystem::path dpath(cfg.BUCKET_DIR_PATH);
    dpath /= MINIMAL_DB_NAME;
    return dpath;
}

void
setupMinimalDBForInMemoryMode(Config const& cfg, uint32_t startAtLedger)
{
    releaseAssertOrThrow(cfg.isInMemoryMode());

    VirtualClock clock;
    Application::pointer app;

    // Look for an existing minimal database, and see if it's possible to
    // restore ledger state from buckets. If it is not possible, reset the
    // existing database back to genesis. If the minimal database does not
    // exist, create a new one.
    bool found = false;

    auto cfgToCheckDB = cfg;
    cfgToCheckDB.METADATA_OUTPUT_STREAM = "";

    if (std::filesystem::exists(minimalDbPath(cfg)))
    {
        app = Application::create(clock, cfgToCheckDB, /* newDB */ false);
        found = true;
    }
    else
    {
        LOG_INFO(DEFAULT_LOG, "Minimal database not found, creating one...");
        app = Application::create(clock, cfgToCheckDB, /* newDB */ true);
    }

    // Rebuild the state from scratch if:
    //  - --start-at-ledger was not provided
    //  - target catchup ledger is before LCL
    //  - target catchup ledger is too far ahead of LCL
    // In all other cases, attempt restoring the ledger states via
    // local bucket application
    if (found)
    {
        LOG_INFO(DEFAULT_LOG, "Found the existing minimal database");

        // DB state might be set to 0 if core previously exited while rebuilding
        // state. In this case, we want to rebuild the DB from scratch
        bool rebuildDB =
            app->getLedgerManager().getLastClosedLedgerHAS().currentLedger <
            LedgerManager::GENESIS_LEDGER_SEQ;

        if (!rebuildDB)
        {
            app->getLedgerManager().loadLastKnownLedger(nullptr);
            auto lcl = app->getLedgerManager().getLastClosedLedgerNum();
            LOG_INFO(DEFAULT_LOG, "Current in-memory state, got LCL: {}", lcl);
            rebuildDB =
                !canRebuildInMemoryLedgerFromBuckets(startAtLedger, lcl);
        }

        if (rebuildDB)
        {
            LOG_INFO(DEFAULT_LOG, "Cannot restore the in-memory state, "
                                  "rebuilding the state from scratch");
            app->resetDBForInMemoryMode();
        }
    }
}

Application::pointer
setupApp(Config& cfg, VirtualClock& clock, uint32_t startAtLedger,
         std::string const& startAtHash)
{
    if (cfg.isInMemoryMode())
    {
        setupMinimalDBForInMemoryMode(cfg, startAtLedger);
    }

    LOG_INFO(DEFAULT_LOG, "Starting stellar-core {}", STELLAR_CORE_VERSION);
    Application::pointer app;
    app = Application::create(clock, cfg, false);
    if (!app->getHistoryArchiveManager().checkSensibleConfig())
    {
        return nullptr;
    }

    app->getLedgerManager().loadLastKnownLedger(nullptr);
    auto lcl = app->getLedgerManager().getLastClosedLedgerHeader();

    if (cfg.isInMemoryMode() &&
        lcl.header.ledgerSeq == LedgerManager::GENESIS_LEDGER_SEQ)
    {
        // If ledger is genesis, rebuild genesis state from buckets
        if (!applyBucketsForLCL(*app))
        {
            return nullptr;
        }
    }

    bool doCatchupForInMemoryMode =
        cfg.isInMemoryMode() && startAtLedger != 0 && !startAtHash.empty();
    if (doCatchupForInMemoryMode)
    {
        // At this point, setupApp has either confirmed that we can rebuild from
        // the existing buckets, or reset the DB to genesis
        if (lcl.header.ledgerSeq != LedgerManager::GENESIS_LEDGER_SEQ)
        {
            auto lclHashStr = binToHex(lcl.hash);
            if (lcl.header.ledgerSeq == startAtLedger &&
                lclHashStr != startAtHash)
            {
                LOG_ERROR(DEFAULT_LOG,
                          "Provided hash {} does not agree with stored hash {}",
                          startAtHash, lclHashStr);
                return nullptr;
            }

            auto has = app->getLedgerManager().getLastClosedLedgerHAS();

            // Collect bucket references to pass to catchup _before_ starting
            // the app, which may trigger garbage collection
            std::set<std::shared_ptr<Bucket>> retained;
            for (auto const& b : has.allBuckets())
            {
                auto bPtr =
                    app->getBucketManager().getBucketByHash(hexToBin256(b));
                releaseAssert(bPtr);
                retained.insert(bPtr);
            }

            // Start the app with LCL set to 0
            app->getLedgerManager().setupInMemoryStateRebuild();
            app->start();

            // Set Herder to track the actual LCL
            app->getHerder().setTrackingSCPState(lcl.header.ledgerSeq,
                                                 lcl.header.scpValue, true);

            // Schedule the catchup work that will rebuild state
            auto cc = CatchupConfiguration(has, lcl);
            app->getLedgerManager().startCatchup(cc, /* archive */ nullptr,
                                                 retained);
        }
        else
        {
            LedgerNumHashPair pair;
            pair.first = startAtLedger;
            pair.second = std::optional<Hash>(hexToBin256(startAtHash));
            auto mode = CatchupConfiguration::Mode::OFFLINE_BASIC;
            Json::Value catchupInfo;
            int res =
                catchup(app, CatchupConfiguration{pair, 0, mode}, catchupInfo,
                        /* archive */ nullptr);
            if (res != 0)
            {
                return nullptr;
            }
        }
    }

    return app;
}

int
runApp(Application::pointer app)
{
    // Certain in-memory modes in core may start the app before reaching this
    // point, but since start is idempotent, second call will just no-op
    app->start();

    // Perform additional startup procedures (must be done after the app is
    // setup) and run the app
    try
    {
        if (!app->getConfig().MODE_AUTO_STARTS_OVERLAY)
        {
            app->getOverlayManager().start();
        }

        app->applyCfgCommands();
    }
    catch (std::exception const& e)
    {
        LOG_FATAL(DEFAULT_LOG, "Got an exception: {}", e.what());
        LOG_FATAL(DEFAULT_LOG, "{}", REPORT_INTERNAL_BUG);
        return 1;
    }

    auto& io = app->getClock().getIOContext();
    asio::io_context::work mainWork(io);
    while (!io.stopped())
    {
        app->getClock().crank();
    }
    return 0;
}

bool
applyBucketsForLCL(Application& app,
                   std::function<bool(LedgerEntryType)> onlyApply)
{
    auto has = app.getLedgerManager().getLastClosedLedgerHAS();
    auto lclHash =
        app.getPersistentState().getState(PersistentState::kLastClosedLedger);

    auto maxProtocolVersion = app.getConfig().LEDGER_PROTOCOL_VERSION;
    auto currentLedger =
        LedgerHeaderUtils::loadByHash(app.getDatabase(), hexToBin256(lclHash));
    if (currentLedger)
    {
        maxProtocolVersion = currentLedger->ledgerVersion;
    }

    std::map<std::string, std::shared_ptr<Bucket>> buckets;
    auto work = app.getWorkScheduler().scheduleWork<ApplyBucketsWork>(
        buckets, has, maxProtocolVersion, onlyApply);

    while (app.getClock().crank(true) && !work->isDone())
        ;

    return work->getState() == BasicWork::State::WORK_SUCCESS;
}

bool
applyBucketsForLCL(Application& app)
{
    return applyBucketsForLCL(app, [](LedgerEntryType) { return true; });
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
        std::cout << ret << std::endl;
    }
    else
    {
        LOG_INFO(DEFAULT_LOG, "http failed({}) port: {} command: {}", code,
                 port, command);
    }
}

void
setAuthenticatedLedgerHashPair(Application::pointer app,
                               LedgerNumHashPair& authPair,
                               uint32_t startLedger, std::string startHash)
{
    auto const& lm = app->getLedgerManager();
    auto const& hm = app->getHistoryManager();

    auto tryCheckpoint = [&](uint32_t seq, Hash h) {
        if (hm.isLastLedgerInCheckpoint(seq))
        {
            LOG_INFO(DEFAULT_LOG,
                     "Found authenticated checkpoint hash {} for ledger {}",
                     hexAbbrev(h), seq);
            authPair.first = seq;
            authPair.second = std::make_optional<Hash>(h);
            return true;
        }
        else if (authPair.first != seq)
        {
            authPair.first = seq;
            LOG_INFO(DEFAULT_LOG,
                     "Ledger {} is not a checkpoint boundary, waiting.", seq);
        }
        return false;
    };

    if (startLedger != 0 && !startHash.empty())
    {
        Hash h = hexToBin256(startHash);
        if (tryCheckpoint(startLedger, h))
        {
            return;
        }
    }

    if (lm.isSynced())
    {
        auto const& lhe = lm.getLastClosedLedgerHeader();
        tryCheckpoint(lhe.header.ledgerSeq, lhe.hash);
    }
    else
    {
        auto lcd = app->getCatchupManager().maybeGetLargestBufferedLedger();
        if (lcd)
        {
            uint32_t seq = lcd->getLedgerSeq() - 1;
            Hash hash = lcd->getTxSet()->previousLedgerHash();
            tryCheckpoint(seq, hash);
        }
    }
}

int
selfCheck(Config cfg)
{
    VirtualClock clock;
    cfg.setNoListen();
    Application::pointer app = Application::create(clock, cfg, false);

    // We run self-checks from a "loaded but dormant" state where the
    // application is not started, but the LM has loaded the LCL.
    app->getLedgerManager().loadLastKnownLedger(nullptr);

    // First we schedule the cheap, asynchronous "online" checks that get run by
    // the HTTP "self-check" endpoint, and crank until they're done.
    LOG_INFO(DEFAULT_LOG, "Self-check phase 1: fast online checks");
    auto seq1 = app->scheduleSelfCheck(false);
    while (clock.crank(true) && !seq1->isDone())
        ;

    // Then we scan all the buckets to check they have expected hashes.
    LOG_INFO(DEFAULT_LOG, "Self-check phase 2: bucket hash verification");
    auto seq2 = app->getBucketManager().scheduleVerifyReferencedBucketsWork();
    while (clock.crank(true) && !seq2->isDone())
        ;

    // Then we load the entire BL ledger state into memory and check it against
    // the database. This part is synchronous and should _not_ be run "online",
    // it's too expensive; it also can't easily be turned _into_ something you
    // can run online, because it would need to snapshot the database for the
    // duration of the run and, for example, sqlite doesn't support lockless
    // snapshotting / MVCC.
    //
    // What we do instead is register a background thread listening for
    // control-C so at least the user can interrupt this if they get impatient.
    asio::signal_set stopSignals(app->getWorkerIOContext(), SIGINT);
#ifdef SIGQUIT
    stopSignals.add(SIGQUIT);
#endif
#ifdef SIGTERM
    stopSignals.add(SIGTERM);
#endif
    stopSignals.async_wait([](asio::error_code const& ec, int sig) {
        if (!ec)
        {
            LOG_INFO(DEFAULT_LOG, "got signal {}, exiting self-check", sig);
            exit(1);
        }
    });

    LOG_INFO(DEFAULT_LOG, "Self-check phase 3: ledger consistency checks");
    BucketListIsConsistentWithDatabase blc(*app);
    bool blcOk = true;
    try
    {
        blc.checkEntireBucketlist();
    }
    catch (std::runtime_error& e)
    {
        LOG_ERROR(DEFAULT_LOG, "Error during bucket-list consistency check: {}",
                  e.what());
        blcOk = false;
    }

    LOG_INFO(DEFAULT_LOG, "Self-check phase 4: crypto benchmarking");
    size_t signPerSec = 0, verifyPerSec = 0;
    SecretKey::benchmarkOpsPerSecond(signPerSec, verifyPerSec, 10000);
    LOG_INFO(DEFAULT_LOG, "Benchmarked {} signatures / sec", signPerSec);
    LOG_INFO(DEFAULT_LOG, "Benchmarked {} verifications / sec", verifyPerSec);

    if (seq1->getState() == BasicWork::State::WORK_SUCCESS &&
        seq2->getState() == BasicWork::State::WORK_SUCCESS && blcOk)
    {
        LOG_INFO(DEFAULT_LOG, "Self-check succeeded");
        return 0;
    }
    else
    {
        LOG_ERROR(DEFAULT_LOG, "Self-check failed");
        return 1;
    }
}

int
mergeBucketList(Config cfg, std::string const& outputDir)
{
    VirtualClock clock;
    cfg.setNoListen();
    Application::pointer app = Application::create(clock, cfg, false);
    app->getLedgerManager().loadLastKnownLedger(nullptr);
    auto& lm = app->getLedgerManager();
    auto& bm = app->getBucketManager();
    HistoryArchiveState has = lm.getLastClosedLedgerHAS();
    auto bucket = bm.mergeBuckets(has);

    using std::filesystem::path;
    path bpath(bucket->getFilename());
    path outpath(outputDir);
    outpath /= bpath.filename();
    if (fs::durableRename(bpath.string(), outpath.string(), outputDir))
    {
        LOG_INFO(DEFAULT_LOG, "Wrote merged bucket {}", outpath);
        return 0;
    }
    else
    {
        LOG_ERROR(DEFAULT_LOG, "Writing bucket failed");
        return 1;
    }
}

int
dumpLedger(Config cfg, std::string const& outputFile,
           std::optional<std::string> filterQuery,
           std::optional<uint32_t> lastModifiedLedgerCount,
           std::optional<uint64_t> limit, std::optional<std::string> groupBy,
           std::optional<std::string> aggregate)
{
    if (groupBy && !aggregate)
    {
        LOG_FATAL(DEFAULT_LOG, "--group-by without --agg is not allowed.");
    }

    VirtualClock clock;
    cfg.setNoListen();
    Application::pointer app = Application::create(clock, cfg, false);
    app->getLedgerManager().loadLastKnownLedger(nullptr);
    auto& lm = app->getLedgerManager();
    HistoryArchiveState has = lm.getLastClosedLedgerHAS();
    std::optional<uint32_t> minLedger;
    if (lastModifiedLedgerCount)
    {
        uint32_t lclNum = lm.getLastClosedLedgerNum();
        if (lclNum >= *lastModifiedLedgerCount)
        {
            minLedger = lclNum - *lastModifiedLedgerCount;
        }
        else
        {
            minLedger = 0;
        }
    }
    std::optional<xdrquery::XDRMatcher> matcher;
    if (filterQuery)
    {
        matcher.emplace(*filterQuery);
    }

    std::optional<xdrquery::XDRFieldExtractor> groupByExtractor;
    if (groupBy)
    {
        groupByExtractor.emplace(*groupBy);
    }

    std::map<std::vector<xdrquery::ResultType>, xdrquery::XDRAccumulator>
        accumulators;

    std::ofstream ofs(outputFile);

    auto& bm = app->getBucketManager();
    uint64_t entryCount = 0;
    try
    {
        bm.visitLedgerEntries(
            has, minLedger,
            [&](LedgerEntry const& entry) {
                return !matcher || matcher->matchXDR(entry);
            },
            [&](LedgerEntry const& entry) {
                if (aggregate)
                {
                    std::vector<xdrquery::ResultType> key;
                    if (groupByExtractor)
                    {
                        key = groupByExtractor->extractFields(entry);
                    }
                    auto it = accumulators.find(key);
                    if (it == accumulators.end())
                    {
                        it = accumulators
                                 .emplace(key,
                                          xdrquery::XDRAccumulator(*aggregate))
                                 .first;
                    }
                    it->second.addEntry(entry);
                }
                else
                {
                    ofs << xdr_to_string(entry, "entry", true) << std::endl;
                }
                ++entryCount;
                return !limit || entryCount < *limit;
            });
    }
    catch (xdrquery::XDRQueryError& e)
    {
        LOG_ERROR(DEFAULT_LOG, "Filter query error: {}", e.what());
    }

    if (aggregate)
    {
        writeLedgerAggregationTable(ofs, groupByExtractor, accumulators);
    }

    LOG_INFO(DEFAULT_LOG, "Finished running query, processed {} entries.",
             entryCount);
    return 0;
}

void
setForceSCPFlag()
{
    LOG_WARNING(DEFAULT_LOG, "* ");
    LOG_WARNING(DEFAULT_LOG,
                "* Nothing to do: `force scp` command has been deprecated");
    LOG_WARNING(DEFAULT_LOG,
                "* Refer to `--wait-for-consensus` run option instead");
    LOG_WARNING(DEFAULT_LOG, "* ");
}

void
initializeDatabase(Config cfg)
{
    VirtualClock clock;
    cfg.setNoListen();
    Application::pointer app = Application::create(clock, cfg);

    LOG_INFO(DEFAULT_LOG, "*");
    LOG_INFO(DEFAULT_LOG,
             "* The next launch will catchup from the network afresh.");
    LOG_INFO(DEFAULT_LOG, "*");
}

void
showOfflineInfo(Config cfg, bool verbose)
{
    // needs real time to display proper stats
    VirtualClock clock(VirtualClock::REAL_TIME);
    cfg.setNoListen();
    Application::pointer app = Application::create(clock, cfg, false);
    app->reportInfo(verbose);
}

#ifdef BUILD_TESTS
void
loadXdr(Config cfg, std::string const& bucketFile)
{
    VirtualClock clock;
    cfg.setNoListen();
    Application::pointer app = Application::create(clock, cfg, false);

    uint256 zero;
    Bucket bucket(bucketFile, zero, nullptr);
    bucket.apply(*app);
}

int
rebuildLedgerFromBuckets(Config cfg)
{
    VirtualClock clock(VirtualClock::REAL_TIME);
    cfg.setNoListen();
    Application::pointer app = Application::create(clock, cfg, false, true);

    app->gracefulStop();
    while (clock.crank(true))
        ;
    return 1;
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
            LOG_INFO(DEFAULT_LOG, "*");
            LOG_INFO(DEFAULT_LOG, "* Last history checkpoint");
            std::cout << state.toString() << std::endl;
            LOG_INFO(DEFAULT_LOG, "*");
        }
        else
        {
            state.save(filename);
            LOG_INFO(DEFAULT_LOG, "*");
            LOG_INFO(DEFAULT_LOG, "* Wrote last history checkpoint {}",
                     filename);
            LOG_INFO(DEFAULT_LOG, "*");
        }
    }
    else
    {
        LOG_INFO(DEFAULT_LOG, "*");
        LOG_INFO(DEFAULT_LOG, "* Fetching last history checkpoint failed.");
        LOG_INFO(DEFAULT_LOG, "*");
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
        LOG_INFO(DEFAULT_LOG, "*");
        LOG_INFO(DEFAULT_LOG, "* Catchup info:");
        std::cout << content << std::endl;
        LOG_INFO(DEFAULT_LOG, "*");
    }
    else
    {
        std::ofstream out{};
        out.exceptions(std::ios::failbit | std::ios::badbit);
        out.open(filename);
        out.write(content.c_str(), content.size());
        out.close();

        LOG_INFO(DEFAULT_LOG, "*");
        LOG_INFO(DEFAULT_LOG, "* Wrote catchup info to {}", filename);
        LOG_INFO(DEFAULT_LOG, "*");
    }
}

int
catchup(Application::pointer app, CatchupConfiguration cc,
        Json::Value& catchupInfo, std::shared_ptr<HistoryArchive> archive)
{
    app->start();

    try
    {
        app->getLedgerManager().startCatchup(cc, archive, {});
    }
    catch (std::invalid_argument const&)
    {
        LOG_INFO(DEFAULT_LOG, "*");
        LOG_INFO(DEFAULT_LOG,
                 "* Target ledger {} is not newer than last closed ledger {} - "
                 "nothing to do",
                 cc.toLedger(),
                 app->getLedgerManager().getLastClosedLedgerNum());
        LOG_INFO(
            DEFAULT_LOG,
            "* If you really want to catchup to {} run stellar-core new-db",
            cc.toLedger());
        LOG_INFO(DEFAULT_LOG, "*");
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

    LOG_INFO(DEFAULT_LOG, "*");
    if (synced)
    {
        LOG_INFO(DEFAULT_LOG, "* Catchup finished.");
    }
    else
    {
        LOG_INFO(DEFAULT_LOG, "* Catchup failed.");
    }
    LOG_INFO(DEFAULT_LOG, "*");

    catchupInfo = app->getJsonInfo(true);
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
    size_t expectedPublishQueueSize = isCheckpoint ? 1 : 0;

    app->getHistoryManager().publishQueuedHistory();
    while (app->getHistoryManager().publishQueueLength() !=
               expectedPublishQueueSize &&
           clock.crank(true))
    {
    }

    // Cleanup buckets not referenced by publish queue anymore
    app->getBucketManager().forgetUnreferencedBuckets();

    LOG_INFO(DEFAULT_LOG, "*");
    LOG_INFO(DEFAULT_LOG, "* Publish finished.");
    LOG_INFO(DEFAULT_LOG, "*");

    return 0;
}

std::string
minimalDBForInMemoryMode(Config const& cfg)
{
    return fmt::format(FMT_STRING("sqlite3://{}"),
                       minimalDbPath(cfg).generic_string());
}
}
