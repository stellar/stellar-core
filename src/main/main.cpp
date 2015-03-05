// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC
#include "util/asio.h"
#include "main/Application.h"
#include "generated/StellardVersion.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "util/TmpDir.h"
#include "lib/util/getopt.h"
#include "main/test.h"
#include "main/Config.h"
#include "lib/http/HttpClient.h"
#include "crypto/SecretKey.h"
#include "history/HistoryMaster.h"
#include "main/PersistentState.h"
#include <sodium.h>
#include "database/Database.h"

_INITIALIZE_EASYLOGGINGPP

namespace stellar 
{


enum opttag
{
    OPT_VERSION = 0x100,
    OPT_HELP,
    OPT_TEST,
    OPT_CONF,
    OPT_CMD,
    OPT_FORCESCP,
    OPT_LOCAL,
    OPT_GENSEED,
    OPT_LOGLEVEL,
    OPT_NEWDB,
    OPT_NEWHIST
};

static const struct option stellard_options[] = {
    {"version", no_argument, nullptr, OPT_VERSION},
    {"help", no_argument, nullptr, OPT_HELP},
    {"test", no_argument, nullptr, OPT_TEST},
    {"conf", required_argument, nullptr, OPT_CONF},
    {"c", required_argument, nullptr, OPT_CMD},
    {"local", no_argument, nullptr, OPT_LOCAL },
    {"genseed", no_argument, nullptr, OPT_GENSEED },
    {"newdb", no_argument, nullptr, OPT_NEWDB },
    {"newhist", required_argument, nullptr, OPT_NEWHIST },
    {"forcescp", no_argument, nullptr, OPT_FORCESCP },
    {"ll", required_argument, nullptr, OPT_LOGLEVEL },
    {nullptr, 0, nullptr, 0}};

static void
usage(int err = 1)
{
    std::ostream& os = err ? std::cerr : std::cout;
    os << "usage: stellard [OPTIONS]\n"
          "where OPTIONS can be any of:\n"
          "      --help          To display this string\n"
          "      --version       To print version information\n"
          "      --test          To run self-tests\n"
          "      --newdb         Setup the DB.\n"
          "      --newhist ARCH  Initialize the named history archive ARCH.\n"
          "      --forcescp      Force SCP to start before you hear a ledger close next time stellard is run.\n"
          "      --local         Resume from locally saved state.\n"
          "      --genseed       Generate and print a random node seed.\n"
          "      --ll LEVEL      Set the log level. LEVEL can be:\n"
          "                      [trace|debug|info|warning|error|fatal|none]\n"
          "      --c             Command to send to local hayashi\n"
          "                stop\n"
          "                info\n"
          "                reload_cfg?file=newconfig.cfg\n"
          "                logrotate\n"
          "                peers\n"
          "                connect?ip=5.5.5.5&port=3424\n"
          "                tx?blob=TX_IN_HEX\n"
          "      --conf FILE     To specify a config file ('-' for STDIN, default "
          "'stellard.cfg')\n";
    exit(err);
}

static void
sendCommand(const std::string& command, const std::vector<char*>& rest,
            int port)
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
    if (app->getPersistentState().getState(PersistentState::kDatabaseInitialized) != "true")
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
        app->getPersistentState().setState(PersistentState::kForceSCPOnNextLaunch, "true");
        LOG(INFO) << "* ";
        LOG(INFO) << "* The `force scp` flag has been set in the db. The next launch will";
        LOG(INFO) << "* and start scp from the account balances as they stand";
        LOG(INFO) << "* in the db now, without waiting to hear from the network.";
        LOG(INFO) << "* ";
    }
}

void            
initializeDatabase(Config &cfg) 
{
    cfg.REBUILD_DB = false; // don't wipe the db until we read whether it was already initialized
    VirtualClock clock;
    Application::pointer app = Application::create(clock, cfg);

    auto wipeMsg = (app->getPersistentState().getState(PersistentState::kDatabaseInitialized) == "true"
        ? " wiped and initialized"
        : " initialized");

    app->getDatabase().initialize();

    LOG(INFO) << "* ";
    LOG(INFO) << "* The database has been" << wipeMsg << ". The next launch will catchup from the";
    LOG(INFO) << "* network afresh.";
    LOG(INFO) << "* ";
}


int
initializeHistories(Config& cfg, vector<string> newHistories)
{
    VirtualClock clock;
    Application::pointer app = Application::create(clock, cfg);

    for (auto const& arch : newHistories)
    {
        if (!HistoryMaster::initializeHistoryArchive(*app, arch))
            return 1;
    }
    return 0;
}

int
startApp(string cfgFile, Config& cfg)
{
    LOG(INFO) << "Starting stellard-hayashi " << STELLARD_VERSION;
    LOG(INFO) << "Config from " << cfgFile;
    VirtualClock clock;
    Application::pointer app = Application::create(clock, cfg);

    if (!checkInitialized(app))
    {
        return 1;
    } else
    {
        app->applyCfgCommands();

        app->start();
        app->enableRealTimer();

        auto& io = app->getMainIOService();
        asio::io_service::work mainWork(io);
        while (!io.stopped())
        {
            app->crank();
        }
        return 1;
    }
}

}

int
main(int argc, char* const* argv)
{
    using namespace stellar;

    try
    {
        sodium_init();
        Logging::init();

        std::string cfgFile("stellard.cfg");
        std::string command;
        el::Level logLevel = el::Level::Fatal;
        std::vector<char*> rest;

        bool newNetwork = false;
        bool localNetwork = false;
        bool newDB = false;
        std::vector<std::string> newHistories;

        int opt;
        while ((opt = getopt_long_only(argc, argv, "", stellard_options,
            nullptr)) != -1)
        {
            switch (opt)
            {
            case OPT_TEST:
            {
                rest.push_back(*argv);
                rest.insert(++rest.begin(), argv + optind, argv + argc);
                return test(static_cast<int>(rest.size()), &rest[0], logLevel);
            }
            case OPT_CONF:
                cfgFile = std::string(optarg);
                break;
            case OPT_CMD:
                command = optarg;
                rest.insert(rest.begin(), argv + optind, argv + argc);
                break;
            case OPT_VERSION:
                std::cout << STELLARD_VERSION;
                return 0;
            case OPT_FORCESCP:
                newNetwork = true;
                break;
            case OPT_NEWDB:
                newDB = true;
                break;
            case OPT_NEWHIST:
                newHistories.push_back(std::string(optarg));
                break;
            case OPT_LOCAL:
                localNetwork = true;
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
        if (TmpDir::exists(cfgFile))
        {
            cfg.load(cfgFile);
            Logging::setLoggingToFile(cfg.LOG_FILE_PATH);
        }
        else
        {
            LOG(WARNING) << "No config file " << cfgFile << " found";
            cfgFile = ":default-settings:";
        }
        Logging::setLogLevel(logLevel, nullptr);

        cfg.REBUILD_DB = newDB;
        cfg.START_NEW_NETWORK = newNetwork;
        cfg.START_LOCAL_NETWORK = localNetwork;
        if (command.size())
        {
            sendCommand(command, rest, cfg.HTTP_PORT);
            return 0;
        }
        else if (newNetwork)
        {
            setForceSCPFlag(cfg);
            return 0;
        }
        else if (newDB)
        {
            initializeDatabase(cfg);
            return 0;
        }
        else if (!newHistories.empty())
        {
            return initializeHistories(cfg, newHistories);
        }
        else
        {
            return startApp(cfgFile, cfg);
        }

    } catch (std::runtime_error e) 
    {
        LOG(FATAL) << e.what();
        return 1;
    }
}

