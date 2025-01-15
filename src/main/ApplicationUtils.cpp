// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/ApplicationUtils.h"
#include "bucket/BucketManager.h"
#include "bucket/LiveBucketList.h"
#include "catchup/ApplyBucketsWork.h"
#include "catchup/CatchupConfiguration.h"
#include "crypto/Hex.h"
#include "database/Database.h"
#include "herder/Herder.h"
#include "herder/QuorumIntersectionChecker.h"
#include "history/HistoryArchive.h"
#include "history/HistoryArchiveManager.h"
#include "history/HistoryArchiveReportWork.h"
#include "historywork/GetHistoryArchiveStateWork.h"
#include "invariant/BucketListIsConsistentWithDatabase.h"
#include "ledger/LedgerHeaderUtils.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTypeUtils.h"
#include "main/ErrorMessages.h"
#include "main/Maintainer.h"
#include "main/PersistentState.h"
#include "main/StellarCoreVersion.h"
#include "overlay/OverlayManager.h"
#include "scp/LocalNode.h"
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
#include <regex>

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
        keyFields = groupByExtractor->getColumnNames();
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

Application::pointer
setupApp(Config& cfg, VirtualClock& clock)
{
    LOG_INFO(DEFAULT_LOG, "Starting stellar-core {}", STELLAR_CORE_VERSION);
    Application::pointer app;
    app = Application::create(clock, cfg, false);
    if (!app->getHistoryArchiveManager().checkSensibleConfig())
    {
        return nullptr;
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
applyBucketsForLCL(Application& app)
{
    auto has = app.getLedgerManager().getLastClosedLedgerHAS();
    auto lclHash = app.getPersistentState().getState(
        PersistentState::kLastClosedLedger, app.getDatabase().getSession());

    auto maxProtocolVersion = app.getConfig().LEDGER_PROTOCOL_VERSION;
    auto currentLedger =
        LedgerHeaderUtils::loadByHash(app.getDatabase(), hexToBin256(lclHash));
    if (currentLedger)
    {
        maxProtocolVersion = currentLedger->ledgerVersion;
    }

    std::map<std::string, std::shared_ptr<LiveBucket>> buckets;
    auto work = app.getWorkScheduler().scheduleWork<ApplyBucketsWork>(
        buckets, has, maxProtocolVersion);

    while (app.getClock().crank(true) && !work->isDone())
        ;

    return work->getState() == BasicWork::State::WORK_SUCCESS;
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

    auto tryCheckpoint = [&](uint32_t seq, Hash h) {
        if (HistoryManager::isLastLedgerInCheckpoint(seq, app->getConfig()))
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
        auto lcd = app->getLedgerApplyManager().maybeGetLargestBufferedLedger();
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
    app->getLedgerManager().loadLastKnownLedger(/* restoreBucketlist */ false);

    // First we schedule the cheap, asynchronous "online" checks that get run by
    // the HTTP "self-check" endpoint, and crank until they're done.
    LOG_INFO(DEFAULT_LOG, "Self-check phase 1: fast online checks");
    auto seq1 = app->scheduleSelfCheck(false);
    while (clock.crank(true) && !seq1->isDone())
        ;

    // Then we scan all the buckets to check they have expected hashes.
    LOG_INFO(DEFAULT_LOG, "Self-check phase 2: bucket hash verification");
    auto seq2 = app->getBucketManager().scheduleVerifyReferencedBucketsWork(
        app->getLedgerManager().getLastClosedLedgerHAS());
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
    auto& lm = app->getLedgerManager();
    auto& bm = app->getBucketManager();

    lm.loadLastKnownLedger(/* restoreBucketlist */ false);
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

// Per-LedgerKey metrics used for dumping archival state
struct StateArchivalMetric
{
    // True if the newest version of the entry is a DEADENTRY
    bool isDead{};

    // Number of bytes that the newest version of the entry occupies in the
    // BucketList
    uint64_t newestBytes{};

    // Number of bytes that all outdated versions of the entry occupy in the
    // BucketList
    uint64_t outdatedBytes{};
};

static void
processArchivalMetrics(
    std::shared_ptr<LiveBucket const> const b,
    UnorderedMap<LedgerKey, StateArchivalMetric>& ledgerEntries,
    UnorderedMap<LedgerKey, std::pair<StateArchivalMetric, uint32_t>>& ttls)
{
    for (LiveBucketInputIterator in(b); in; ++in)
    {
        auto const& be = *in;
        bool isDead = be.type() == DEADENTRY;
        LedgerKey k = isDead ? be.deadEntry() : LedgerEntryKey(be.liveEntry());
        bool isTTL = k.type() == TTL;

        if (!isTemporaryEntry(k) && !isTTL)
        {
            continue;
        }

        if (isTTL)
        {
            auto iter = ttls.find(k);
            if (iter == ttls.end())
            {
                StateArchivalMetric metric;
                metric.isDead = isDead;
                metric.newestBytes = xdr::xdr_size(be);
                if (isDead)
                {
                    ttls.emplace(k, std::make_pair(metric, 0));
                }
                else
                {
                    ttls.emplace(
                        k, std::make_pair(
                               metric,
                               be.liveEntry().data.ttl().liveUntilLedgerSeq));
                }
            }
            else
            {
                iter->second.first.outdatedBytes += xdr::xdr_size(be);
            }
        }
        else
        {
            auto iter = ledgerEntries.find(k);
            if (iter == ledgerEntries.end())
            {
                StateArchivalMetric metric;
                metric.isDead = isDead;
                metric.newestBytes = xdr::xdr_size(be);
                ledgerEntries.emplace(k, metric);
            }
            else
            {
                iter->second.outdatedBytes += xdr::xdr_size(be);
            }
        }
    }
}

int
dumpStateArchivalStatistics(Config cfg)
{
    ZoneScoped;
    VirtualClock clock;
    cfg.setNoListen();
    Application::pointer app = Application::create(clock, cfg, false);
    app->getLedgerManager().loadLastKnownLedger(/* restoreBucketlist */ false);
    auto& lm = app->getLedgerManager();
    auto& bm = app->getBucketManager();
    HistoryArchiveState has = lm.getLastClosedLedgerHAS();

    std::vector<Hash> hashes;
    for (uint32_t i = 0; i < LiveBucketList::kNumLevels; ++i)
    {
        HistoryStateBucket const& hsb = has.currentBuckets.at(i);
        hashes.emplace_back(hexToBin256(hsb.curr));
        hashes.emplace_back(hexToBin256(hsb.snap));
    }

    UnorderedMap<LedgerKey, StateArchivalMetric> ledgerEntries;

    // key -> (metric, liveUntilLedger)
    UnorderedMap<LedgerKey, std::pair<StateArchivalMetric, uint32_t>> ttls;
    float blSize = 0;
    for (auto const& hash : hashes)
    {
        if (isZero(hash))
        {
            continue;
        }
        auto b = bm.getBucketByHash<LiveBucket>(hash);
        if (!b)
        {
            throw std::runtime_error(std::string("missing bucket: ") +
                                     binToHex(hash));
        }
        processArchivalMetrics(b, ledgerEntries, ttls);
        blSize += b->getSize();
    }

    // *BytesNewest == bytes consumed only by newest version of BucketEntry
    // *BytesOutdated == bytes consumed only by outdated version of BucketEntry
    // live -> liveUntilLedger >= ledgerSeq
    // expired -> liveUntilLedger < ledgerSeq, but not yet evicted
    uint64_t liveBytesNewest{};
    uint64_t liveBytesOutdated{};
    uint64_t expiredBytesNewest{};
    uint64_t expiredBytesOutdated{};
    uint64_t evictedBytes{}; // All evicted bytes considered "outdated"

    for (auto const& [k, leMetric] : ledgerEntries)
    {
        auto ttlIter = ttls.find(getTTLKey(k));
        releaseAssertOrThrow(ttlIter != ttls.end());
        auto const& [ttlMetric, liveUntilLedger] = ttlIter->second;

        auto newestBytes = ttlMetric.newestBytes + leMetric.newestBytes;
        auto outdatedBytes = ttlMetric.outdatedBytes + leMetric.outdatedBytes;

        if (ttlMetric.isDead)
        {
            releaseAssertOrThrow(leMetric.isDead);

            // All bytes considered outdated for evicted entries
            evictedBytes += newestBytes + outdatedBytes;
        }
        else
        {
            releaseAssertOrThrow(!leMetric.isDead);

            // If entry is live
            if (liveUntilLedger >=
                app->getLedgerManager().getLastClosedLedgerNum())
            {
                liveBytesNewest += newestBytes;
                liveBytesOutdated += outdatedBytes;
            }
            else
            {
                expiredBytesNewest += newestBytes;
                expiredBytesOutdated += outdatedBytes;
            }
        }
    }

    CLOG_INFO(Bucket, "Live BucketList total bytes: {}", blSize);
    CLOG_INFO(Bucket,
              "Live Temporary Entries: Newest bytes {} ({}%), Outdated bytes "
              "{} ({}%)",
              liveBytesNewest, (liveBytesNewest / blSize) * 100,
              liveBytesOutdated, (liveBytesOutdated / blSize) * 100);
    CLOG_INFO(Bucket,
              "Expired but not evicted Temporary: Newest bytes {} ({}%), "
              "Outdated bytes {} ({}%)",
              expiredBytesNewest, (expiredBytesNewest / blSize) * 100,
              expiredBytesOutdated, (expiredBytesOutdated / blSize) * 100);
    CLOG_INFO(Bucket, "Evicted Temporary Entries: Outdated bytes {} ({}%)",
              evictedBytes, (evictedBytes / blSize) * 100);

    return 0;
}

int
dumpLedger(Config cfg, std::string const& outputFile,
           std::optional<std::string> filterQuery,
           std::optional<uint32_t> lastModifiedLedgerCount,
           std::optional<uint64_t> limit, std::optional<std::string> groupBy,
           std::optional<std::string> aggregate, bool includeAllStates)
{
    if (groupBy && !aggregate)
    {
        LOG_FATAL(DEFAULT_LOG, "--group-by without --agg is not allowed.");
    }

    VirtualClock clock;
    cfg.setNoListen();
    Application::pointer app = Application::create(clock, cfg, false);
    auto& lm = app->getLedgerManager();

    lm.loadLastKnownLedger(/* restoreBucketlist */ false);
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
                    ofs << xdrToCerealString(entry, "entry", true) << std::endl;
                }
                ++entryCount;
                return !limit || entryCount < *limit;
            },
            includeAllStates);
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

bool
checkQuorumIntersectionFromJson(std::string const& jsonPath,
                                std::optional<Config> const& cfg)
{
    std::ifstream in(jsonPath);
    if (!in)
    {
        throw std::runtime_error("Could not open file '" + jsonPath + "'");
    }
    Json::Reader rdr;
    Json::Value quorumJson;
    if (!rdr.parse(in, quorumJson) || !quorumJson.isObject())
    {
        throw std::runtime_error("Failed to parse '" + jsonPath +
                                 "' as a JSON object");
    }

    Json::Value const& nodesJson = quorumJson["nodes"];
    if (!nodesJson.isArray())
    {
        throw std::runtime_error("JSON field 'nodes' must be an array");
    }

    QuorumIntersectionChecker::QuorumSetMap qmap;
    for (Json::Value const& nodeJson : nodesJson)
    {
        if (!nodeJson["node"].isString())
        {
            throw std::runtime_error("JSON field 'node' must be a string");
        }
        NodeID id = KeyUtils::fromStrKey<NodeID>(nodeJson["node"].asString());
        auto elemPair =
            qmap.try_emplace(id, std::make_shared<SCPQuorumSet>(
                                     LocalNode::fromJson(nodeJson["qset"])));
        if (!elemPair.second)
        {
            throw std::runtime_error(
                "JSON contains multiple nodes with the same 'node' value");
        }
    }

    std::atomic<bool> interrupt(false);
    auto qicPtr =
        QuorumIntersectionChecker::create(qmap, cfg, interrupt, false);

    return qicPtr->networkEnjoysQuorumIntersection();
}

#ifdef BUILD_TESTS
void
loadXdr(Config cfg, std::string const& bucketFile)
{
    VirtualClock clock;
    cfg.setNoListen();
    Application::pointer app = Application::create(clock, cfg, false);

    uint256 zero;
    LiveBucket bucket(bucketFile, zero, nullptr);
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
        switch (app->getLedgerApplyManager().getCatchupWorkState())
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
    auto isCheckpoint =
        HistoryManager::isLastLedgerInCheckpoint(lcl, app->getConfig());
    size_t expectedPublishQueueSize = isCheckpoint ? 1 : 0;

    app->getHistoryManager().publishQueuedHistory();
    while (HistoryManager::publishQueueLength(app->getConfig()) !=
               expectedPublishQueueSize &&
           clock.crank(true))
    {
    }

    // Cleanup buckets not referenced by publish queue anymore
    app->getBucketManager().forgetUnreferencedBuckets(
        app->getLedgerManager().getLastClosedLedgerHAS());

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

// Returns the major release version extracted from the git tag _if_ this is a
// release-tagged version of stellar core (one that looks like vNN.X.Y or
// vNN.X.YrcZ or vNN.X.YHOTZ). If its version has some other name structure
// structure, return std::nullopt.
std::optional<uint32_t>
getStellarCoreMajorReleaseVersion(std::string const& vstr)
{
    std::regex re("^v([0-9]+)\\.[0-9]+\\.[0-9]+(rc[0-9]+|HOT[0-9]+)?$");
    std::smatch match;
    if (std::regex_match(vstr, match, re))
    {
        uint32_t vers = stoi(match.str(1));
        return std::make_optional<uint32_t>(vers);
    }
    return std::nullopt;
}

}
