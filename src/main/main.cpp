// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#include "util/asio.h"
#include "main/Application.h"
#include "generated/StellarCoreVersion.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "util/Fs.h"
#include "lib/util/getopt.h"
#include "main/test.h"
#include "main/Config.h"
#include "lib/http/HttpClient.h"
#include "crypto/Hex.h"
#include "crypto/SecretKey.h"
#include "history/HistoryManager.h"
#include "main/PersistentState.h"
#include <sodium.h>
#include "database/Database.h"

_INITIALIZE_EASYLOGGINGPP

namespace stellar
{

using namespace std;

enum opttag
{
    OPT_VERSION = 0x100,
    OPT_HELP,
    OPT_TEST,
    OPT_CONF,
    OPT_CMD,
    OPT_FORCESCP,
    OPT_GENSEED,
    OPT_LOGLEVEL,
    OPT_METRIC,
    OPT_NEWDB,
    OPT_NEWHIST
};

static const struct option stellar_core_options[] = {
    {"version", no_argument, nullptr, OPT_VERSION},
    {"help", no_argument, nullptr, OPT_HELP},
    {"test", no_argument, nullptr, OPT_TEST},
    {"conf", required_argument, nullptr, OPT_CONF},
    {"c", required_argument, nullptr, OPT_CMD},
    {"genseed", no_argument, nullptr, OPT_GENSEED},
    {"metric", required_argument, nullptr, OPT_METRIC},
    {"newdb", no_argument, nullptr, OPT_NEWDB},
    {"newhist", required_argument, nullptr, OPT_NEWHIST},
    {"forcescp", no_argument, nullptr, OPT_FORCESCP},
    {"ll", required_argument, nullptr, OPT_LOGLEVEL},
    {nullptr, 0, nullptr, 0}};

static void
usage(int err = 1)
{
    std::ostream& os = err ? std::cerr : std::cout;
    os << "usage: stellar-core [OPTIONS]\n"
          "where OPTIONS can be any of:\n"
          "      --help          To display this string\n"
          "      --version       To print version information\n"
          "      --test          To run self-tests\n"
          "      --metric METRIC Report metric METRIC on exit.\n"
          "      --newdb         Creates or restores the DB to the genesis "
          "ledger\n"
          "      --newhist ARCH  Initialize the named history archive ARCH\n"
          "      --forcescp      Force SCP to start with the local ledger as "
          "position, close next time stellar-core is run\n"
          "      --genseed       Generate and print a random node seed\n"
          "      --ll LEVEL      Set the log level. LEVEL can be:\n"
          "                      [trace|debug|info|warning|error|fatal|none]\n"
          "      --c             Command to send to local stellar-core. try "
          "'--c help' for more information\n"
          "      --conf FILE     To specify a config file ('-' for STDIN, "
          "default 'stellar-core.cfg')\n";
    exit(err);
}

static void
sendCommand(const std::string& command, const std::vector<char*>& rest,
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
setForceSCPFlag(Config& cfg)
{
    VirtualClock clock;
    Application::pointer app = Application::create(clock, cfg);

    if (checkInitialized(app))
    {
        app->getPersistentState().setState(
            PersistentState::kForceSCPOnNextLaunch, "true");
        LOG(INFO) << "* ";
        LOG(INFO) << "* The `force scp` flag has been set in the db.";
        LOG(INFO) << "* ";
        LOG(INFO)
            << "* The next launch will start scp from the account balances";
        LOG(INFO)
            << "* as they stand in the db now, without waiting to hear from";
        LOG(INFO) << "* the network.";
        LOG(INFO) << "* ";
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

    cfg.REBUILD_DB = false;
}

int
initializeHistories(Config& cfg, vector<string> newHistories)
{
    // prevent opening up a port for other peers
    cfg.RUN_STANDALONE = true;
    cfg.HTTP_PORT = 0;
    cfg.MANUAL_CLOSE = true;

    VirtualClock clock;
    Application::pointer app = Application::create(clock, cfg);

    for (auto const& arch : newHistories)
    {
        if (!HistoryManager::initializeHistoryArchive(*app, arch))
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
    Application::pointer app = Application::create(clock, cfg);

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

        auto& io = clock.getIOService();
        asio::io_service::work mainWork(io);
        while (!io.stopped())
        {
            clock.crank();
        }
        return 0;
    }
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

    bool forceSCP = false;
    bool newDB = false;
    std::vector<std::string> newHistories;
    std::vector<std::string> metrics;

    int opt;
    while ((opt = getopt_long_only(argc, argv, "", stellar_core_options,
                                   nullptr)) != -1)
    {
        switch (opt)
        {
        case OPT_TEST:
        {
            rest.push_back(*argv);
            rest.insert(++rest.begin(), argv + optind, argv + argc);
            return test(static_cast<int>(rest.size()), &rest[0], logLevel,
                        metrics);
        }
        case OPT_CONF:
            cfgFile = std::string(optarg);
            break;
        case OPT_CMD:
            command = optarg;
            rest.insert(rest.begin(), argv + optind, argv + argc);
            break;
        case OPT_VERSION:
            std::cout << STELLAR_CORE_VERSION;
            return 0;
        case OPT_FORCESCP:
            forceSCP = true;
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
        case OPT_LOGLEVEL:
            logLevel = Logging::getLLfromString(std::string(optarg));
            break;
        case OPT_GENSEED:
        {
            SecretKey key = SecretKey::random();
            std::cout << "Secret seed: " << key.getBase58Seed() << std::endl;
            std::cout << "Public: " << key.getBase58Public() << std::endl;
            return 0;
        }

        default:
            usage(0);
            return 0;
        }
    }

    Config cfg;
    try
    {
        if (fs::exists(cfgFile))
        {
            cfg.load(cfgFile);
        }
        else
        {
            LOG(WARNING) << "No config file " << cfgFile << " found";
            cfgFile = ":default-settings:";
        }
        Logging::setFmt(hexAbbrev(cfg.PEER_PUBLIC_KEY));
        Logging::setLogLevel(logLevel, nullptr);

        if (command.size())
        {
            sendCommand(command, rest, cfg.HTTP_PORT);
            return 0;
        }

        // don't log to file if just sending a command
        Logging::setLoggingToFile(cfg.LOG_FILE_PATH);
        Logging::setLogLevel(logLevel, nullptr);

        cfg.REBUILD_DB = newDB;
        cfg.FORCE_SCP = forceSCP;
        cfg.REPORT_METRICS = metrics;

        if (forceSCP || newDB)
        {
            if (newDB)
                initializeDatabase(cfg);
            if (forceSCP)
                setForceSCPFlag(cfg);
            return 0;
        }
        else if (!newHistories.empty())
        {
            return initializeHistories(cfg, newHistories);
        }
    }
    catch (std::invalid_argument& e)
    {
        LOG(FATAL) << e.what();
        return 1;
    }
    // run outside of catch block so that we properly capture crashes
    return startApp(cfgFile, cfg);
}
