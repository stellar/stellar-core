// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#include "util/asio.h"
#include "bucket/Bucket.h"
#include "bucket/BucketManager.h"
#include "catchup/CatchupConfiguration.h"
#include "catchup/CatchupManager.h"
#include "catchup/CatchupWork.h"
#include "catchup/VerifyLedgerChainWork.h"
#include "crypto/Hex.h"
#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "database/Database.h"
#include "history/HistoryManager.h"
#include "historywork/GetHistoryArchiveStateWork.h"
#include "ledger/LedgerManager.h"
#include "lib/http/HttpClient.h"
#include "lib/util/getopt.h"
#include "main/Application.h"
#include "main/Config.h"
#include "main/ExternalQueue.h"
#include "main/Maintainer.h"
#include "main/PersistentState.h"
#include "main/StellarCoreVersion.h"
#include "main/dumpxdr.h"
#include "main/fuzz.h"
#include "test/test.h"
#include "util/Fs.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "util/optional.h"
#include "work/WorkManager.h"
#include <lib/util/format.h>
#include <limits>
#include <locale>
#include <sodium.h>

INITIALIZE_EASYLOGGINGPP

namespace stellar
{

using namespace std;

enum opttag
{
    OPT_CATCHUP_AT,
    OPT_CATCHUP_COMPLETE,
    OPT_CATCHUP_RECENT,
    OPT_CATCHUP_TO,
    OPT_CMD,
    OPT_CONF,
    OPT_CONVERTID,
    OPT_CHECKQUORUM,
    OPT_BASE64,
    OPT_DUMPXDR,
    OPT_LOADXDR,
    OPT_FORCESCP,
    OPT_FUZZ,
    OPT_GENFUZZ,
    OPT_GENSEED,
    OPT_GRAPHQUORUM,
    OPT_HELP,
    OPT_INFERQUORUM,
    OPT_OFFLINEINFO,
    OPT_OUTPUT_FILE,
    OPT_REPORT_LAST_HISTORY_CHECKPOINT,
    OPT_LOGLEVEL,
    OPT_METRIC,
    OPT_NEWDB,
    OPT_NEWHIST,
    OPT_PRINTTXN,
    OPT_SEC2PUB,
    OPT_SIGNTXN,
    OPT_NETID,
    OPT_TEST,
    OPT_VERSION
};

static const struct option stellar_core_options[] = {
    {"catchup-at", required_argument, nullptr, OPT_CATCHUP_AT},
    {"catchup-complete", no_argument, nullptr, OPT_CATCHUP_COMPLETE},
    {"catchup-recent", required_argument, nullptr, OPT_CATCHUP_RECENT},
    {"catchup-to", required_argument, nullptr, OPT_CATCHUP_TO},
    {"c", required_argument, nullptr, OPT_CMD},
    {"conf", required_argument, nullptr, OPT_CONF},
    {"convertid", required_argument, nullptr, OPT_CONVERTID},
    {"checkquorum", optional_argument, nullptr, OPT_CHECKQUORUM},
    {"base64", no_argument, nullptr, OPT_BASE64},
    {"dumpxdr", required_argument, nullptr, OPT_DUMPXDR},
    {"printtxn", required_argument, nullptr, OPT_PRINTTXN},
    {"signtxn", required_argument, nullptr, OPT_SIGNTXN},
    {"netid", required_argument, nullptr, OPT_NETID},
    {"loadxdr", required_argument, nullptr, OPT_LOADXDR},
    {"forcescp", optional_argument, nullptr, OPT_FORCESCP},
    {"fuzz", required_argument, nullptr, OPT_FUZZ},
    {"genfuzz", required_argument, nullptr, OPT_GENFUZZ},
    {"genseed", no_argument, nullptr, OPT_GENSEED},
    {"graphquorum", optional_argument, nullptr, OPT_GRAPHQUORUM},
    {"help", no_argument, nullptr, OPT_HELP},
    {"inferquorum", optional_argument, nullptr, OPT_INFERQUORUM},
    {"offlineinfo", no_argument, nullptr, OPT_OFFLINEINFO},
    {"output-file", required_argument, nullptr, OPT_OUTPUT_FILE},
    {"report-last-history-checkpoint", no_argument, nullptr,
     OPT_REPORT_LAST_HISTORY_CHECKPOINT},
    {"sec2pub", no_argument, nullptr, OPT_SEC2PUB},
    {"ll", required_argument, nullptr, OPT_LOGLEVEL},
    {"metric", required_argument, nullptr, OPT_METRIC},
    {"newdb", no_argument, nullptr, OPT_NEWDB},
    {"newhist", required_argument, nullptr, OPT_NEWHIST},
    {"test", no_argument, nullptr, OPT_TEST},
    {"version", no_argument, nullptr, OPT_VERSION},
    {nullptr, 0, nullptr, 0}};

static void
usage(int err = 1)
{
    std::ostream& os = err ? std::cerr : std::cout;
    os << "usage: stellar-core [OPTIONS]\n"
          "where OPTIONS can be any of:\n"
          "      --base64             Use base64 for --printtxn and --signtxn\n"
          "      --catchup-at SEQ     Do a catchup at ledger SEQ, then quit\n"
          "                           Use current as SEQ to catchup to "
          "'current'"
          "history checkpoint\n"
          "      --catchup-complete   Do a complete catchup, then quit\n"
          "      --catchup-recent NUM Do a recent catchup for NUM ledgers, "
          "then quit\n"
          "      --catchup-to SEQ     Do a catchup to ledger SEQ, then quit\n"
          "                           Use current as SEQ to catchup to "
          "'current'"
          "history checkpoint\n"
          "      --c                  Send a command to local stellar-core. "
          "try "
          "'--c help' for more information\n"
          "      --conf FILE          Specify a config file ('-' for STDIN, "
          "default 'stellar-core.cfg')\n"
          "      --convertid ID       Displays ID in all known forms\n"
          "      --dumpxdr FILE       Dump an XDR file, for debugging\n"
          "      --loadxdr FILE       Load an XDR bucket file, for testing\n"
          "      --forcescp           Next time stellar-core is run, SCP will "
          "start "
          "with the local ledger rather than waiting to hear from the "
          "network.\n"
          "      --fuzz FILE          Run a single fuzz input and exit\n"
          "      --genfuzz FILE       Generate a random fuzzer input file\n"
          "      --genseed            Generate and print a random node seed\n"
          "      --help               Display this string\n"
          "      --inferquorum        Print a quorum set inferred from "
          "history\n"
          "      --checkquorum        Check quorum intersection from history\n"
          "      --graphquorum        Print a quorum set graph from history\n"
          "      --output-file        Output file for --graphquorum and "
          "--report-last-history-checkpoint commands\n"
          "      --offlineinfo        Return information for an offline "
          "instance\n"
          "      --ll LEVEL           Set the log level. (redundant with --c "
          "ll "
          "but "
          "you need this form for the tests.)\n"
          "                           LEVEL can be: trace, debug, info, error, "
          "fatal\n"
          "      --metric METRIC      Report metric METRIC on exit\n"
          "      --newdb              Creates or restores the DB to the "
          "genesis "
          "ledger\n"
          "      --newhist ARCH       Initialize the named history archive "
          "ARCH\n"
          "      --printtxn FILE      Pretty-print one transaction envelope,"
          " then quit\n"
          "      --report-last-history-checkpoint\n"
          "                           Report information about last checkpoint "
          "available in history archives\n"
          "      --signtxn FILE       Add signature to transaction envelope,"
          " then quit\n"
          "                           (Key is read from stdin or terminal, as"
          " appropriate.)\n"
          "      --sec2pub            Print the public key corresponding to a "
          "secret key\n"
          "      --netid STRING       Specify network ID for subsequent "
          "signtxn\n"
          "                           (Default is STELLAR_NETWORK_ID "
          "environment variable)\n"
          "      --test               Run self-tests\n"
          "      --version            Print version information\n";
    exit(err);
}

static void
setNoListen(Config& cfg)
{
    // prevent opening up a port for other peers
    cfg.RUN_STANDALONE = true;
    cfg.HTTP_PORT = 0;
    cfg.MANUAL_CLOSE = true;
}

static void
sendCommand(std::string const& command, const std::vector<char*>& rest,
            unsigned short port)
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

static bool
checkInitialized(Application::pointer app)
{
    try
    {
        // check to see if the state table exists
        app->getPersistentState().getState(PersistentState::kDatabaseSchema);
    }
    catch (...)
    {
        LOG(INFO) << "* ";
        LOG(INFO) << "* The database has not yet been initialized. Try --newdb";
        LOG(INFO) << "* ";
        return false;
    }
    return true;
}

static int
catchup(Application::pointer app, uint32_t to, uint32_t count,
        Json::Value& catchupInfo)
{
    if (!checkInitialized(app))
    {
        return 1;
    }

    auto done = false;
    app->getLedgerManager().loadLastKnownLedger(
        [&done](asio::error_code const& ec) {
            if (ec)
            {
                throw std::runtime_error(
                    "Unable to restore last-known ledger state");
            }

            done = true;
        });
    auto& clock = app->getClock();
    while (!done && clock.crank(true))
        ;

    try
    {
        app->getLedgerManager().startCatchUp({to, count}, true);
    }
    catch (std::invalid_argument const&)
    {
        LOG(INFO) << "*";
        LOG(INFO) << "* Target ledger " << to
                  << " is not newer than last closed ledger"
                  << " - nothing to do";
        LOG(INFO) << "* If you really want to catchup to " << to
                  << " run stellar-core with --newdb parameter.";
        LOG(INFO) << "*";
        return 2;
    }

    auto& io = clock.getIOService();
    auto synced = false;
    asio::io_service::work mainWork(io);
    done = false;
    while (!done && clock.crank(true))
    {
        switch (app->getLedgerManager().getState())
        {
        case LedgerManager::LM_BOOTING_STATE:
        {
            LOG(INFO) << "*";
            LOG(INFO) << "* Catchup failed.";
            LOG(INFO) << "*";
            done = true;
            break;
        }
        case LedgerManager::LM_SYNCED_STATE:
        {
            LOG(INFO) << "*";
            LOG(INFO) << "* Catchup finished.";
            LOG(INFO) << "*";
            done = true;
            synced = true;
            break;
        }
        case LedgerManager::LM_CATCHING_UP_STATE:
            break;
        }
    }

    catchupInfo = app->getJsonInfo();
    return synced ? 0 : 3;
}

static int
catchupAt(Application::pointer app, uint32_t at, Json::Value& catchupInfo)
{
    return catchup(app, at, 0, catchupInfo);
}

static int
catchupComplete(Application::pointer app, Json::Value& catchupInfo)
{
    return catchup(app, CatchupConfiguration::CURRENT,
                   std::numeric_limits<uint32_t>::max(), catchupInfo);
}

static int
catchupRecent(Application::pointer app, uint32_t count,
              Json::Value& catchupInfo)
{
    return catchup(app, CatchupConfiguration::CURRENT, count, catchupInfo);
}

static int
catchupTo(Application::pointer app, uint32_t to, Json::Value& catchupInfo)
{
    return catchup(app, to, std::numeric_limits<uint32_t>::max(), catchupInfo);
}

static void
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
        out.open(filename);
        out.write(content.c_str(), content.size());
        out.close();

        LOG(INFO) << "*";
        LOG(INFO) << "* Wrote catchup info to " << filename;
        LOG(INFO) << "*";
    }
}

static int
reportLastHistoryCheckpoint(Config const& cfg, std::string const& outputFile)
{
    VirtualClock clock(VirtualClock::REAL_TIME);
    Application::pointer app = Application::create(clock, cfg, false);

    if (!checkInitialized(app))
    {
        return 1;
    }

    auto state = HistoryArchiveState{};
    auto& wm = app->getWorkManager();
    auto getHistoryArchiveStateWork =
        wm.executeWork<GetHistoryArchiveStateWork>(
            true, "get-history-archive-state-work", state);

    auto ok = getHistoryArchiveStateWork->getState() == Work::WORK_SUCCESS;
    if (ok)
    {
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

static uint32_t
parseLedger(std::string const& str)
{
    if (str == "current")
    {
        return CatchupConfiguration::CURRENT;
    }

    auto pos = std::size_t{0};
    auto result = std::stoul(str, &pos);
    if (pos < str.length() || result < 2)
    {
        throw std::runtime_error(
            fmt::format("{} is not a valid ledger number", str));
    }

    return result;
}

static uint32_t
parseLedgerCount(std::string const& str)
{
    auto pos = std::size_t{0};
    auto result = std::stoul(str, &pos);
    if (pos < str.length())
    {
        throw std::runtime_error(
            fmt::format("{} is not a valid ledger count", str));
    }

    return result;
}

static void
setForceSCPFlag(Config const& cfg, bool isOn)
{
    VirtualClock clock;
    Application::pointer app = Application::create(clock, cfg, false);

    if (checkInitialized(app))
    {
        app->getPersistentState().setState(
            PersistentState::kForceSCPOnNextLaunch, (isOn ? "true" : "false"));
        if (isOn)
        {
            LOG(INFO) << "* ";
            LOG(INFO) << "* The `force scp` flag has been set in the db.";
            LOG(INFO) << "* ";
            LOG(INFO)
                << "* The next launch will start scp from the account balances";
            LOG(INFO) << "* as they stand in the db now, without waiting to "
                         "hear from";
            LOG(INFO) << "* the network.";
            LOG(INFO) << "* ";
        }
        else
        {
            LOG(INFO) << "* ";
            LOG(INFO) << "* The `force scp` flag has been cleared.";
            LOG(INFO) << "* The next launch will start normally.";
            LOG(INFO) << "* ";
        }
    }
}

static void
showOfflineInfo(Config const& cfg)
{
    // needs real time to display proper stats
    VirtualClock clock(VirtualClock::REAL_TIME);
    Application::pointer app = Application::create(clock, cfg, false);
    if (checkInitialized(app))
    {
        app->reportInfo();
    }
    else
    {
        LOG(INFO) << "Database is not initialized";
    }
}

static void
loadXdr(Config const& cfg, std::string const& bucketFile)
{
    VirtualClock clock;
    Application::pointer app = Application::create(clock, cfg, false);
    if (checkInitialized(app))
    {
        uint256 zero;
        Bucket bucket(bucketFile, zero);
        bucket.apply(app->getDatabase());
    }
    else
    {
        LOG(INFO) << "Database is not initialized";
    }
}

static void
inferQuorumAndWrite(Config const& cfg)
{
    InferredQuorum iq;
    {
        VirtualClock clock;
        Application::pointer app = Application::create(clock, cfg, false);
        iq = app->getHistoryManager().inferQuorum();
    }
    LOG(INFO) << "Inferred quorum";
    std::cout << iq.toString(cfg) << std::endl;
}

static void
checkQuorumIntersection(Config const& cfg)
{
    VirtualClock clock;
    Application::pointer app = Application::create(clock, cfg, false);
    InferredQuorum iq = app->getHistoryManager().inferQuorum();
    iq.checkQuorumIntersection(cfg);
}

static void
writeQuorumGraph(Config const& cfg, std::string const& outputFile)
{
    InferredQuorum iq;
    {
        VirtualClock clock;
        Application::pointer app = Application::create(clock, cfg, false);
        iq = app->getHistoryManager().inferQuorum();
    }
    std::string filename = outputFile.empty() ? "-" : outputFile;
    if (filename == "-")
    {
        std::stringstream out;
        iq.writeQuorumGraph(cfg, out);
        LOG(INFO) << "*";
        LOG(INFO) << "* Quorum graph: " << out.str();
        LOG(INFO) << "*";
    }
    else
    {
        std::ofstream out(filename);
        iq.writeQuorumGraph(cfg, out);
        LOG(INFO) << "*";
        LOG(INFO) << "* Wrote quorum graph to " << filename;
        LOG(INFO) << "*";
    }
}

static void
initializeDatabase(Config& cfg)
{
    VirtualClock clock;
    Application::pointer app = Application::create(clock, cfg);

    LOG(INFO) << "*";
    LOG(INFO) << "* The next launch will catchup from the network afresh.";
    LOG(INFO) << "*";
}

static int
initializeHistories(Config& cfg, vector<string> newHistories)
{
    VirtualClock clock;
    Application::pointer app = Application::create(clock, cfg, false);

    for (auto const& arch : newHistories)
    {
        if (!HistoryManager::initializeHistoryArchive(*app, arch))
            return 1;
    }
    return 0;
}

static int
startApp(string cfgFile, Config& cfg)
{
    LOG(INFO) << "Starting stellar-core " << STELLAR_CORE_VERSION;
    LOG(INFO) << "Config from " << cfgFile;
    VirtualClock clock(VirtualClock::REAL_TIME);
    Application::pointer app;
    try
    {
        app = Application::create(clock, cfg, false);

        if (!checkInitialized(app))
        {
            return 0;
        }
        else
        {
            if (!HistoryManager::checkSensibleConfig(cfg))
            {
                return 1;
            }
            if (cfg.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING)
            {
                LOG(WARNING) << "Artificial acceleration of time enabled "
                             << "(for testing only)";
            }

            app->applyCfgCommands();

            app->start();
        }
    }
    catch (std::exception& e)
    {
        LOG(FATAL) << "Got an exception: " << e.what();
        return 1;
    }
    auto& io = clock.getIOService();
    asio::io_service::work mainWork(io);
    while (!io.stopped())
    {
        clock.crank();
    }
    return 0;
}
}

int
main(int argc, char* const* argv)
{
    using namespace stellar;

    Logging::init();
    if (sodium_init() != 0)
    {
        LOG(FATAL) << "Could not initialize crypto";
        return 1;
    }
    xdr::marshaling_stack_limit = 1000;

    std::string cfgFile("stellar-core.cfg");
    std::string command;
    el::Level logLevel = el::Level::Info;
    std::vector<char*> rest;

    optional<bool> forceSCP = nullptr;
    bool base64 = false;
    bool doCatchupAt = false;
    uint32_t catchupAtTarget = 0;
    bool doCatchupComplete = false;
    bool doCatchupRecent = false;
    uint32_t catchupRecentCount = 0;
    bool doCatchupTo = false;
    uint32_t catchupToTarget = 0;
    bool inferQuorum = false;
    bool checkQuorum = false;
    bool graphQuorum = false;
    bool newDB = false;
    bool getOfflineInfo = false;
    auto doReportLastHistoryCheckpoint = false;
    std::string outputFile;
    std::string loadXdrBucket;
    std::vector<std::string> newHistories;
    std::vector<std::string> metrics;

    int opt;
    while ((opt = getopt_long_only(argc, argv, "c:", stellar_core_options,
                                   nullptr)) != -1)
    {
        switch (opt)
        {
        case OPT_BASE64:
            base64 = true;
            break;
        case OPT_CATCHUP_AT:
            doCatchupAt = true;
            catchupAtTarget = parseLedger(optarg);
            break;
        case OPT_CATCHUP_COMPLETE:
            doCatchupComplete = true;
            break;
        case OPT_CATCHUP_RECENT:
            doCatchupRecent = true;
            catchupRecentCount = parseLedgerCount(optarg);
            break;
        case OPT_CATCHUP_TO:
            doCatchupTo = true;
            catchupToTarget = parseLedger(optarg);
            break;
        case 'c':
        case OPT_CMD:
            command = optarg;
            rest.insert(rest.begin(), argv + optind, argv + argc);
            optind = argc;
            break;
        case OPT_CONF:
            cfgFile = std::string(optarg);
            break;
        case OPT_CONVERTID:
            StrKeyUtils::logKey(std::cout, std::string(optarg));
            return 0;
        case OPT_DUMPXDR:
            dumpxdr(std::string(optarg));
            return 0;
        case OPT_PRINTTXN:
            printtxn(std::string(optarg), base64);
            return 0;
        case OPT_SIGNTXN:
            signtxn(std::string(optarg), base64);
            return 0;
        case OPT_SEC2PUB:
            priv2pub();
            return 0;
        case OPT_NETID:
            signtxn_network_id = optarg;
            return 0;
        case OPT_LOADXDR:
            loadXdrBucket = std::string(optarg);
            break;
        case OPT_FORCESCP:
            forceSCP = make_optional<bool>(optarg == nullptr ||
                                           string(optarg) == "true");
            break;
        case OPT_FUZZ:
            fuzz(std::string(optarg), logLevel, metrics);
            return 0;
        case OPT_GENFUZZ:
            genfuzz(std::string(optarg));
            return 0;
        case OPT_GENSEED:
        {
            SecretKey key = SecretKey::random();
            std::cout << "Secret seed: " << key.getStrKeySeed().value
                      << std::endl;
            std::cout << "Public: " << key.getStrKeyPublic() << std::endl;
            return 0;
        }
        case OPT_INFERQUORUM:
            inferQuorum = true;
            break;
        case OPT_CHECKQUORUM:
            checkQuorum = true;
            break;
        case OPT_GRAPHQUORUM:
            graphQuorum = true;
            break;
        case OPT_OFFLINEINFO:
            getOfflineInfo = true;
            break;
        case OPT_OUTPUT_FILE:
            outputFile = optarg;
            break;
        case OPT_LOGLEVEL:
            logLevel = Logging::getLLfromString(std::string(optarg));
            break;
        case OPT_METRIC:
            metrics.push_back(std::string(optarg));
            break;
        case OPT_NEWDB:
            newDB = true;
            break;
        case OPT_NEWHIST:
            newHistories.push_back(std::string(optarg));
            break;
        case OPT_REPORT_LAST_HISTORY_CHECKPOINT:
            doReportLastHistoryCheckpoint = true;
            break;
        case OPT_TEST:
        {
            rest.push_back(*argv);
            rest.insert(++rest.begin(), argv + optind, argv + argc);
            return test(static_cast<int>(rest.size()), &rest[0], logLevel,
                        metrics);
        }
        case OPT_VERSION:
            std::cout << STELLAR_CORE_VERSION << std::endl;
            return 0;
        case OPT_HELP:
        default:
            usage(0);
            return 0;
        }
    }

    Config cfg;
    try
    {
        // yes you really have to do this 3 times
        Logging::setLogLevel(logLevel, nullptr);
        if (cfgFile == "-" || fs::exists(cfgFile))
        {
            cfg.load(cfgFile);
        }
        else
        {
            std::string s;
            s = "No config file ";
            s += cfgFile + " found";
            throw std::invalid_argument(s);
        }
        Logging::setFmt(KeyUtils::toShortString(cfg.NODE_SEED.getPublicKey()));
        Logging::setLogLevel(logLevel, nullptr);

        if (command.size())
        {
            sendCommand(command, rest, cfg.HTTP_PORT);
            return 0;
        }

        // don't log to file if just sending a command
        if (cfg.LOG_FILE_PATH.size())
            Logging::setLoggingToFile(cfg.LOG_FILE_PATH);
        Logging::setLogLevel(logLevel, nullptr);

        cfg.REPORT_METRICS = metrics;

        if (forceSCP || newDB || getOfflineInfo || !loadXdrBucket.empty() ||
            inferQuorum || graphQuorum || checkQuorum || doCatchupAt ||
            doCatchupComplete || doCatchupRecent || doCatchupTo ||
            doReportLastHistoryCheckpoint)
        {
            auto result = 0;
            setNoListen(cfg);
            if (newDB)
                initializeDatabase(cfg);
            if ((result == 0) && (doCatchupAt || doCatchupComplete ||
                                  doCatchupRecent || doCatchupTo))
            {
                Json::Value catchupInfo;
                VirtualClock clock(VirtualClock::REAL_TIME);
                auto app = Application::create(clock, cfg, false);
                // set known cursors before starting maintenance job
                ExternalQueue ps(*app);
                ps.setInitialCursors(cfg.KNOWN_CURSORS);
                app->getMaintainer().start();
                if (doCatchupAt)
                    result = catchupAt(app, catchupAtTarget, catchupInfo);
                if ((result == 0) && doCatchupComplete)
                    result = catchupComplete(app, catchupInfo);
                if ((result == 0) && doCatchupRecent)
                    result =
                        catchupRecent(app, catchupRecentCount, catchupInfo);
                if ((result == 0) && doCatchupTo)
                    result = catchupTo(app, catchupToTarget, catchupInfo);
                app->gracefulStop();
                while (app->getClock().crank(true))
                    ;
                if (!catchupInfo.isNull())
                    writeCatchupInfo(catchupInfo, outputFile);
            }
            if ((result == 0) && forceSCP)
                setForceSCPFlag(cfg, *forceSCP);
            if ((result == 0) && getOfflineInfo)
                showOfflineInfo(cfg);
            if ((result == 0) && doReportLastHistoryCheckpoint)
                result = reportLastHistoryCheckpoint(cfg, outputFile);
            if ((result == 0) && !loadXdrBucket.empty())
                loadXdr(cfg, loadXdrBucket);
            if ((result == 0) && inferQuorum)
                inferQuorumAndWrite(cfg);
            if ((result == 0) && checkQuorum)
                checkQuorumIntersection(cfg);
            if ((result == 0) && graphQuorum)
                writeQuorumGraph(cfg, outputFile);
            return result;
        }
        else if (!newHistories.empty())
        {
            setNoListen(cfg);
            return initializeHistories(cfg, newHistories);
        }

        if (cfg.MANUAL_CLOSE)
        {
            // in manual close mode, we set FORCE_SCP
            // so that the node starts fully in sync
            // (this is to avoid to force scp all the time when testing)
            cfg.FORCE_SCP = true;
        }
    }
    catch (std::exception& e)
    {
        LOG(FATAL) << "Got an exception: " << e.what();
        return 1;
    }
    // run outside of catch block so that we properly capture crashes
    return startApp(cfgFile, cfg);
}
