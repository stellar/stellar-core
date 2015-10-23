// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#include "util/asio.h"
#include "main/Application.h"
#include "StellarCoreVersion.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "util/Fs.h"
#include "lib/util/getopt.h"
#include "main/dumpxdr.h"
#include "main/fuzz.h"
#include "main/test.h"
#include "main/Config.h"
#include "lib/http/HttpClient.h"
#include "crypto/Hex.h"
#include "crypto/SecretKey.h"
#include "history/HistoryManager.h"
#include "main/PersistentState.h"
#include <sodium.h>
#include "database/Database.h"
#include "bucket/Bucket.h"
#include "util/optional.h"

_INITIALIZE_EASYLOGGINGPP

namespace stellar
{

using namespace std;

enum opttag
{
    OPT_CMD,
    OPT_CONF,
    OPT_CONVERTID,
    OPT_DUMPXDR,
    OPT_LOADXDR,
    OPT_FORCESCP,
    OPT_FUZZ,
    OPT_GENFUZZ,
    OPT_GENSEED,
    OPT_HELP,
    OPT_INFO,
    OPT_LOGLEVEL,
    OPT_METRIC,
    OPT_NEWDB,
    OPT_NEWHIST,
    OPT_TEST,
    OPT_VERSION
};

static const struct option stellar_core_options[] = {
    {"c", required_argument, nullptr, OPT_CMD},
    {"conf", required_argument, nullptr, OPT_CONF},
    {"convertid", required_argument, nullptr, OPT_CONVERTID},
    {"dumpxdr", required_argument, nullptr, OPT_DUMPXDR},
    {"loadxdr", required_argument, nullptr, OPT_LOADXDR},
    {"forcescp", optional_argument, nullptr, OPT_FORCESCP},
    {"fuzz", required_argument, nullptr, OPT_FUZZ},
    {"genfuzz", required_argument, nullptr, OPT_GENFUZZ},
    {"genseed", no_argument, nullptr, OPT_GENSEED},
    {"help", no_argument, nullptr, OPT_HELP},
    {"info", no_argument, nullptr, OPT_INFO},
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
          "      --c             Command to send to local stellar-core. try "
          "'--c help' for more information\n"
          "      --conf FILE     To specify a config file ('-' for STDIN, "
          "default 'stellar-core.cfg')\n"
          "      --convertid ID  Displays ID in all known forms\n"
          "      --dumpxdr FILE  To dump an XDR file, for debugging\n"
          "      --loadxdr FILE  To load an XDR bucket file, for testing\n"
          "      --forcescp      Next time stellar-core is run, SCP will start "
          "with the local ledger rather than waiting to hear from the "
          "network.\n"
          "      --fuzz FILE     To run a single fuzz input and exit\n"
          "      --genfuzz FILE  Generate a random fuzzer input file\n "
          "      --genseed       Generate and print a random node seed\n"
          "      --help          To display this string\n"
          "      --info          Returns some information on the instance\n"
          "      --ll LEVEL      Set the log level. (redundant with --c ll but "
          "you need this form for the tests.)\n"
          "                      LEVEL can be:\n"
          "      --metric METRIC Report metric METRIC on exit\n"
          "      --newdb         Creates or restores the DB to the genesis "
          "ledger\n"
          "      --newhist ARCH  Initialize the named history archive ARCH\n"
          "      --test          To run self-tests\n"
          "      --version       To print version information\n";
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
    path << "/" << command;

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

bool
checkInitialized(Application::pointer app)
{
    if (app->getPersistentState().getState(
            PersistentState::kDatabaseInitialized) != "true")
    {
        LOG(INFO) << "* ";
        LOG(INFO) << "* The database has not yet been initialized. Try --newdb";
        LOG(INFO) << "* ";
        return false;
    }
    return true;
}

void
setForceSCPFlag(Config const& cfg, bool isOn)
{
    VirtualClock clock;
    Application::pointer app = Application::create(clock, cfg);

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

void
showInfo(Config const& cfg)
{
    // needs real time to display proper stats
    VirtualClock clock(VirtualClock::REAL_TIME);
    Application::pointer app = Application::create(clock, cfg);
    if (checkInitialized(app))
    {
        app->reportInfo();
    }
    else
    {
        LOG(INFO) << "Database is not initialized";
    }
}

void
loadXdr(Config const& cfg, std::string const& bucketFile)
{
    VirtualClock clock;
    Application::pointer app = Application::create(clock, cfg);
    if (checkInitialized(app))
    {
        uint256 zero;
        Bucket bucket(bucketFile, zero);
        bucket.setRetain(true);
        bucket.apply(app->getDatabase());
    }
    else
    {
        LOG(INFO) << "Database is not initialized";
    }
}

void
initializeDatabase(Config& cfg)
{
    VirtualClock clock;
    Application::pointer app = Application::create(clock, cfg);

    LOG(INFO) << "*";
    LOG(INFO) << "* The next launch will catchup from the network afresh.";
    LOG(INFO) << "*";
}

int
initializeHistories(Config& cfg, vector<string> newHistories)
{
    VirtualClock clock;
    Application::pointer app = Application::create(clock, cfg);

    for (auto const& arch : newHistories)
    {
        if(!HistoryManager::initializeHistoryArchive(*app, arch))
                    return 1;           
    }
    return 0;
}

int
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

    sodium_init();
    Logging::init();

    std::string cfgFile("stellar-core.cfg");
    std::string command;
    el::Level logLevel = el::Level::Info;
    std::vector<char*> rest;

    optional<bool> forceSCP = nullptr;
    bool newDB = false;
    bool getInfo = false;
    std::string loadXdrBucket = "";
    std::vector<std::string> newHistories;
    std::vector<std::string> metrics;

    int opt;
    while ((opt = getopt_long_only(argc, argv, "", stellar_core_options,
                                   nullptr)) != -1)
    {
        switch (opt)
        {
        case OPT_CMD:
            command = optarg;
            rest.insert(rest.begin(), argv + optind, argv + argc);
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
            std::cout << "Secret seed: " << key.getStrKeySeed() << std::endl;
            std::cout << "Public: " << key.getStrKeyPublic() << std::endl;
            return 0;
        }
        case OPT_INFO:
            getInfo = true;
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
        case OPT_TEST:
        {
            rest.push_back(*argv);
            rest.insert(++rest.begin(), argv + optind, argv + argc);
            return test(static_cast<int>(rest.size()), &rest[0], logLevel,
                        metrics);
        }
        case OPT_VERSION:
            std::cout << STELLAR_CORE_VERSION;
            return 0;
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
        Logging::setFmt(cfg.toShortString(cfg.NODE_SEED.getPublicKey()));
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

        if (forceSCP || newDB || getInfo || !loadXdrBucket.empty())
        {
            setNoListen(cfg);
            if (newDB)
                initializeDatabase(cfg);
            if (forceSCP)
                setForceSCPFlag(cfg, *forceSCP);
            if (getInfo)
                showInfo(cfg);
            if (!loadXdrBucket.empty())
                loadXdr(cfg, loadXdrBucket);
            return 0;
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
