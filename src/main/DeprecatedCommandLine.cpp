// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/DeprecatedCommandLine.h"
#include "bucket/Bucket.h"
#include "history/HistoryArchiveManager.h"
#include "history/InferredQuorumUtils.h"
#include "historywork/GetHistoryArchiveStateWork.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "main/ApplicationUtils.h"
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
#include "util/optional.h"
#include "work/WorkManager.h"

#include <lib/util/format.h>
#include <lib/util/getopt.h>
#include <string>

namespace stellar
{

namespace
{

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
    OPT_PRINTXDR,
    OPT_SEC2PUB,
    OPT_SIGNTXN,
    OPT_NETID,
    OPT_TEST,
    OPT_FILETYPE,
    OPT_VERSION
};

const struct option stellar_core_options[] = {
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
    {"printxdr", required_argument, nullptr, OPT_PRINTXDR},
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

void
usage(int err = 1)
{
    std::ostream& os = err ? std::cerr : std::cout;
    os << "usage: stellar-core [OPTIONS]\n"
          "where OPTIONS can be any of:\n"
          "      --base64             Use base64 for --printtxn and --signtxn\n"
          "      --catchup-at SEQ     Do a catchup at ledger SEQ, then quit\n"
          "                           Use current as SEQ to catchup to "
          "'current' history checkpoint\n"
          "      --catchup-complete   Do a complete catchup, then quit\n"
          "      --catchup-recent NUM Do a recent catchup for NUM ledgers, "
          "then quit\n"
          "      --catchup-to SEQ     Do a catchup to ledger SEQ, then quit\n"
          "                           Use current as SEQ to catchup to "
          "'current' history checkpoint\n"
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
          "      --printxdr FILE      Pretty print XDR content from FILE, "
          "then quit\n"
          "      --filetype "
          "[auto|ledgerheader|meta|result|resultpair|tx|txfee] toggle for type "
          "used for printxdr\n"
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

int
catchupAt(Application::pointer app, uint32_t at, Json::Value& catchupInfo)
{
    return catchup(app, CatchupConfiguration{at, 0}, catchupInfo);
}

int
catchupComplete(Application::pointer app, Json::Value& catchupInfo)
{
    return catchup(app,
                   CatchupConfiguration{CatchupConfiguration::CURRENT,
                                        std::numeric_limits<uint32_t>::max()},
                   catchupInfo);
}

int
catchupRecent(Application::pointer app, uint32_t count,
              Json::Value& catchupInfo)
{
    return catchup(app,
                   CatchupConfiguration{CatchupConfiguration::CURRENT, count},
                   catchupInfo);
}

int
catchupTo(Application::pointer app, uint32_t to, Json::Value& catchupInfo)
{
    return catchup(
        app, CatchupConfiguration{to, std::numeric_limits<uint32_t>::max()},
        catchupInfo);
}
}

int
handleDeprecatedCommandLine(int argc, char* const* argv)
{
    std::cerr << "Using DEPRECATED command-line syntax." << std::endl;
    std::cerr << "Please refer to documentation for new syntax." << std::endl
              << std::endl;

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
    std::string filetype = "auto";
    std::string netId;

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
            dumpXdrStream(std::string(optarg));
            return 0;
        case OPT_PRINTXDR:
            printXdr(std::string(optarg), filetype, base64);
            return 0;
        case OPT_FILETYPE:
            filetype = std::string(optarg);
            break;
        case OPT_SIGNTXN:
            signtxn(std::string(optarg), netId, base64);
            return 0;
        case OPT_SEC2PUB:
            priv2pub();
            return 0;
        case OPT_NETID:
            netId = optarg;
            return 0;
        case OPT_LOADXDR:
            loadXdrBucket = std::string(optarg);
            break;
        case OPT_FORCESCP:
            forceSCP = make_optional<bool>(optarg == nullptr ||
                                           std::string(optarg) == "true");
            break;
        case OPT_FUZZ:
            fuzz(std::string(optarg), logLevel, metrics);
            return 0;
        case OPT_GENFUZZ:
            genfuzz(std::string(optarg));
            return 0;
        case OPT_GENSEED:
        {
            genSeed();
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
        cfg.load(cfgFile);

        Logging::setFmt(KeyUtils::toShortString(cfg.NODE_SEED.getPublicKey()));
        Logging::setLogLevel(logLevel, nullptr);

        if (command.size())
        {
            httpCommand(command, cfg.HTTP_PORT);
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
            if (newDB)
                initializeDatabase(cfg);
            if ((result == 0) && (doCatchupAt || doCatchupComplete ||
                                  doCatchupRecent || doCatchupTo))
            {
                Json::Value catchupInfo;
                VirtualClock clock(VirtualClock::REAL_TIME);
                cfg.setNoListen();
                auto app = Application::create(clock, cfg, false);
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
            return initializeHistories(cfg, newHistories);
        }
    }
    catch (std::exception& e)
    {
        LOG(FATAL) << "Got an exception: " << e.what();
        return 1;
    }
    // run outside of catch block so that we properly capture crashes
    return runWithConfig(cfg);
}
}
