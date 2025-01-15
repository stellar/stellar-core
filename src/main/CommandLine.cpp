// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// clang-format off
// This needs to be included first
#include "rust/RustVecXdrMarshal.h"
// clang-format on

#include "main/CommandLine.h"
#include "bucket/BucketManager.h"
#include "bucket/LiveBucketList.h"
#include "catchup/CatchupConfiguration.h"
#include "catchup/CatchupRange.h"
#include "catchup/ReplayDebugMetaWork.h"
#include "crypto/SHA.h"
#include "herder/Herder.h"
#include "history/HistoryArchiveManager.h"
#include "historywork/BatchDownloadWork.h"
#include "historywork/WriteVerifiedCheckpointHashesWork.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "main/ApplicationUtils.h"
#include "main/Config.h"
#include "main/Diagnostics.h"
#include "main/ErrorMessages.h"
#include "main/PersistentState.h"
#include "main/SettingsUpgradeUtils.h"
#include "main/StellarCoreVersion.h"
#include "main/dumpxdr.h"
#include "medida/metrics_registry.h"
#include "overlay/OverlayManager.h"
#include "rust/RustBridge.h"
#include "scp/QuorumSetUtils.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include "util/types.h"
#include "work/WorkScheduler.h"

#include <catch.hpp>
#include <cereal/archives/json.hpp>
#include <cereal/cereal.hpp>
#include <cereal/types/vector.hpp>

#ifdef BUILD_TESTS
#include "simulation/ApplyLoad.h"
#include "test/Fuzzer.h"
#include "test/fuzz.h"
#include "test/test.h"
#endif

#include <fmt/format.h>
#include <iostream>
#include <lib/clara.hpp>
#include <optional>

namespace stellar
{

static const uint32_t MAINTENANCE_LEDGER_COUNT = 1000000;

void
writeWithTextFlow(std::ostream& os, std::string const& text)
{
    size_t consoleWidth = CLARA_TEXTFLOW_CONFIG_CONSOLE_WIDTH;
    os << clara::TextFlow::Column(text).width(consoleWidth) << "\n\n";
}

namespace
{

class CommandLine
{
  public:
    struct ConfigOption
    {
        using Common = std::pair<std::string, bool>;
        static const std::vector<Common> COMMON_OPTIONS;

        LogLevel mLogLevel{LogLevel::LVL_INFO};
        std::vector<std::string> mMetrics;
        std::string mConfigFile;
        bool mConsoleLog{false};

        Config getConfig(bool logToFile = true) const;
    };

    class Command
    {
      public:
        using RunFunc = std::function<int(CommandLineArgs const& args)>;

        Command(std::string const& name, std::string const& description,
                RunFunc const& runFunc);
        int run(CommandLineArgs const& args) const;
        std::string name() const;
        std::string description() const;

      private:
        std::string mName;
        std::string mDescription;
        RunFunc mRunFunc;
    };

    explicit CommandLine(std::vector<Command> const& commands);

    using AdjustedCommandLine =
        std::pair<std::string, std::vector<std::string>>;
    AdjustedCommandLine adjustCommandLine(clara::detail::Args const& args);
    std::optional<Command> selectCommand(std::string const& commandName);
    void writeToStream(std::string const& exeName, std::ostream& os) const;

  private:
    std::vector<Command> mCommands;
};

const std::vector<std::pair<std::string, bool>>
    CommandLine::ConfigOption::COMMON_OPTIONS{{"--conf", true},
                                              {"--ll", true},
                                              {"--metric", true},
                                              {"--help", false},
                                              {"--console", false}};

class ParserWithValidation
{
  public:
    ParserWithValidation(
        clara::Parser parser,
        std::function<std::string()> isValid = [] { return std::string{}; })
    {
        mParser = parser;
        mIsValid = isValid;
    }

    ParserWithValidation(
        clara::Arg arg,
        std::function<std::string()> isValid = [] { return std::string{}; })
    {
        mParser = clara::Parser{} | arg;
        mIsValid = isValid;
    }

    ParserWithValidation(
        clara::Opt opt,
        std::function<std::string()> isValid = [] { return std::string{}; })
    {
        mParser = clara::Parser{} | opt;
        mIsValid = isValid;
    }

    const clara::Parser&
    parser() const
    {
        return mParser;
    }

    std::string
    validate() const
    {
        return mIsValid();
    }

  private:
    clara::Parser mParser;
    std::function<std::string()> mIsValid;
};

template <typename T>
std::function<std::string()>
required(T& value, std::string const& name)
{
    return [&value, name] {
        if (value.empty())
        {
            return name + " argument is required";
        }
        else
        {
            return std::string{};
        };
    };
}

template <typename T>
ParserWithValidation
requiredArgParser(T& value, std::string const& name)
{
    return {clara::Arg(value, name).required(), required(value, name)};
}

ParserWithValidation
fileNameParser(std::string& string)
{
    return requiredArgParser(string, "FILE-NAME");
}

clara::Opt
processIDParser(int& num)
{
    return clara::Opt{num, "PROCESS-ID"}["--process-id"](
        "for spawning multiple instances in fuzzing parallelization");
}

clara::Opt
outputFileParser(std::string& string)
{
    return clara::Opt{string, "FILE-NAME"}["--output-file"]("output file");
}

clara::Opt
trustedHashFileParser(std::optional<std::string>& string)
{
    return clara::Opt{[&](std::string const& arg) { string = arg; },
                      "FILE-NAME"}["--trusted-hash-file"](
        "file containing trusted hashes, generated by a previous call to "
        "verify-checkpoints");
}

clara::Opt
outputDirParser(std::string& string)
{
    return clara::Opt{string, "DIR-NAME"}["--output-dir"]("output dir");
}

clara::Opt
metaDirParser(std::string& string)
{
    return clara::Opt{string, "DIR-NAME"}["--meta-dir"](
        "external directory that contains debug meta");
}

clara::Opt
logLevelParser(LogLevel& value)
{
    return clara::Opt{
        [&](std::string const& arg) { value = Logging::getLLfromString(arg); },
        "LEVEL"}["--ll"]("set the log level");
}

clara::Opt
consoleParser(bool& console)
{
    return clara::Opt{console}["--console"]("enable logging to console");
}

clara::Opt
metricsParser(std::vector<std::string>& value)
{
    return clara::Opt{value, "METRIC-NAME"}["--metric"](
        "report metric METRIC-NAME on exit");
}

clara::Opt
compactParser(bool& compact)
{
    return clara::Opt{compact}["--compact"]("no indent");
}

clara::Opt
base64Parser(bool& base64)
{
    return clara::Opt{base64}["--base64"]("batch process base64 encoded input");
}

clara::Opt
codeParser(std::string& code)
{
    return clara::Opt{code, "CODE"}["--code"]("asset code");
}

clara::Opt
issuerParser(std::string& issuer)
{
    return clara::Opt{issuer, "ISSUER"}["--issuer"]("asset issuer");
}

clara::Opt
disableBucketGCParser(bool& disableBucketGC)
{
    return clara::Opt{disableBucketGC}["--disable-bucket-gc"](
        "keeps all, even old, buckets on disk");
}

clara::Opt
waitForConsensusParser(bool& waitForConsensus)
{
    return clara::Opt{waitForConsensus}["--wait-for-consensus"](
        "wait to hear from the network before voting, for validating nodes "
        "only");
}

clara::Opt
historyArchiveParser(std::string& archive)
{
    return clara::Opt(archive, "ARCHIVE-NAME")["--archive"](
        "Archive name to be used for catchup. Use 'any' to select randomly");
}

clara::Opt
historyLedgerNumber(uint32_t& ledgerNum)
{
    return clara::Opt{ledgerNum, "HISTORY-LEDGER"}["--history-ledger"](
        "specify a ledger number to examine in history");
}

clara::Opt
fromLedgerNumberParser(std::optional<uint32_t>& fromLedgerNum)
{
    return clara::Opt{
        [&](std::string const& arg) { fromLedgerNum = std::stoul(arg); },
        "FROM-LEDGER"}["--from-ledger"](
        "specify a ledger number to start from");
}

clara::Opt
historyHashParser(std::string& hash)
{
    return clara::Opt(hash, "HISTORY_HASH")["--history-hash"](
        "specify a hash to trust for the provided ledger");
}

clara::Parser
configurationParser(CommandLine::ConfigOption& configOption)
{
    return logLevelParser(configOption.mLogLevel) |
           metricsParser(configOption.mMetrics) |
           consoleParser(configOption.mConsoleLog) |
           clara::Opt{configOption.mConfigFile,
                      "FILE-NAME"}["--conf"](fmt::format(
               FMT_STRING("specify a config file ('{}' for STDIN, default "
                          "'stellar-core.cfg')"),
               Config::STDIN_SPECIAL_NAME));
}

clara::Opt
metadataOutputStreamParser(std::string& stream)
{
    return clara::Opt(stream, "STREAM")["--metadata-output-stream"](
        "Filename or file-descriptor number 'fd:N' to stream metadata to");
}

void
maybeSetMetadataOutputStream(Config& cfg, std::string const& stream)
{
    if (!stream.empty())
    {
        if (!cfg.METADATA_OUTPUT_STREAM.empty())
        {
            throw std::runtime_error(
                "Command-line --metadata-output-stream conflicts with "
                "config-file provided METADATA_OUTPUT_STREAM");
        }
        cfg.METADATA_OUTPUT_STREAM = stream;
    }
}

clara::Opt
ledgerHashParser(std::string& ledgerHash)
{
    return clara::Opt{ledgerHash, "HASH"}["--trusted-hash"](
        "Hash of the ledger to catchup to");
}

clara::Opt
forceUntrustedCatchup(bool& force)
{
    return clara::Opt{force}["--force-untrusted-catchup"](
        "force unverified catchup");
}

clara::Opt
inMemoryParser(bool& inMemory)
{
    return clara::Opt{inMemory}["--in-memory"](
        "(DEPRECATED) flag is ignored and will be removed soon.");
}

clara::Opt
startAtLedgerParser(uint32_t& startAtLedger)
{
    return clara::Opt{startAtLedger, "LEDGER"}["--start-at-ledger"](
        "(DEPRECATED) flag is ignored and will be removed soon.");
}

clara::Opt
startAtHashParser(std::string& startAtHash)
{
    return clara::Opt{startAtHash, "HASH"}["--start-at-hash"](
        "(DEPRECATED) flag is ignored and will be removed soon.");
}

clara::Opt
filterQueryParser(std::optional<std::string>& filterQuery)
{
    return clara::Opt{[&](std::string const& arg) { filterQuery = arg; },
                      "FILTER-QUERY"}["--filter-query"](
        "query to filter ledger entries");
}

clara::Opt
lastModifiedLedgerCountParser(
    std::optional<std::uint32_t>& lastModifiedLedgerCount)
{
    return clara::Opt{[&](std::string const& arg) {
                          lastModifiedLedgerCount = std::stoul(arg);
                      },
                      "LAST-LEDGERS"}["--last-ledgers"](
        "filter out ledger entries that were modified more than this many "
        "ledgers ago");
}

clara::Opt
groupByParser(std::optional<std::string>& groupBy)
{
    return clara::Opt{[&](std::string const& arg) { groupBy = arg; },
                      "GROUP-BY-EXPR"}["--group-by"](
        "comma-separated fields to group the results by");
}

clara::Opt
aggregateParser(std::optional<std::string>& aggregate)
{
    return clara::Opt{[&](std::string const& arg) { aggregate = arg; },
                      "AGGREGATE-EXPR"}["--agg"](
        "comma-separated aggregate expressions");
}

clara::Opt
limitParser(std::optional<std::uint64_t>& limit)
{
    return clara::Opt{[&](std::string const& arg) { limit = std::stoull(arg); },
                      "LIMIT"}["--limit"](
        "process only this many recent ledger entries (not *most* recent)");
}

clara::Opt
includeAllStatesParser(bool& include)
{
    return clara::Opt{include}["--include-all-states"](
        "include all non-dead states of the entry into query results");
}

int
runWithHelp(CommandLineArgs const& args,
            std::vector<ParserWithValidation> parsers, std::function<int()> f)
{
    auto isHelp = false;
    auto parser = clara::Parser{} | clara::Help(isHelp);
    for (auto const& p : parsers)
        parser |= p.parser();
    auto errorMessage =
        parser
            .parse(args.mCommandName,
                   clara::detail::TokenStream{std::begin(args.mArgs),
                                              std::end(args.mArgs)})
            .errorMessage();
    if (errorMessage.empty())
    {
        for (auto const& p : parsers)
        {
            errorMessage = p.validate();
            if (!errorMessage.empty())
            {
                break;
            }
        }
    }

    if (!errorMessage.empty())
    {
        writeWithTextFlow(std::cerr, errorMessage);
        writeWithTextFlow(std::cerr, args.mCommandDescription);
        parser.writeToStream(std::cerr);
        return 1;
    }

    if (isHelp)
    {
        writeWithTextFlow(std::cout, args.mCommandDescription);
        parser.writeToStream(std::cout);
        return 0;
    }

    return f();
}

CatchupConfiguration
parseCatchup(std::string const& catchup, std::string const& hash,
             bool extraValidation)
{
    auto static errorMessage =
        "catchup value should be passed as <DESTINATION-LEDGER/LEDGER-COUNT>, "
        "where destination ledger is any valid number or 'current' and ledger "
        "count is any valid number or 'max'";

    auto separatorIndex = catchup.find("/");
    if (separatorIndex == std::string::npos)
    {
        throw std::runtime_error(errorMessage);
    }

    try
    {
        auto mode = extraValidation
                        ? CatchupConfiguration::Mode::OFFLINE_COMPLETE
                        : CatchupConfiguration::Mode::OFFLINE_BASIC;
        auto ledger = parseLedger(catchup.substr(0, separatorIndex));
        auto count = parseLedgerCount(catchup.substr(separatorIndex + 1));
        if (hash.empty())
        {
            return CatchupConfiguration(ledger, count, mode);
        }
        else
        {
            return CatchupConfiguration(
                {ledger, std::make_optional<Hash>(hexToBin256(hash))}, count,
                mode);
        }
    }
    catch (std::exception&)
    {
        throw std::runtime_error(errorMessage);
    }
}

CommandLine::Command::Command(std::string const& name,
                              std::string const& description,
                              RunFunc const& runFunc)
    : mName{name}, mDescription{description}, mRunFunc{runFunc}
{
}

int
CommandLine::Command::run(CommandLineArgs const& args) const
{
    return mRunFunc(args);
}

std::string
CommandLine::Command::name() const
{
    return mName;
}

std::string
CommandLine::Command::description() const
{
    return mDescription;
}

Config
CommandLine::ConfigOption::getConfig(bool logToFile) const
{
    Config config;
    auto configFile =
        mConfigFile.empty() ? std::string{"stellar-core.cfg"} : mConfigFile;

    // yes you really have to do this 3 times
    Logging::setLogLevel(mLogLevel, nullptr);
    LOG_INFO(DEFAULT_LOG, "Config from {}", configFile);
    config.load(configFile);

    Logging::setFmt(KeyUtils::toShortString(config.NODE_SEED.getPublicKey()));
    Logging::setLogLevel(mLogLevel, nullptr);

    if (logToFile)
    {
        if (!config.LOG_FILE_PATH.empty())
        {
            Logging::setLoggingToFile(config.LOG_FILE_PATH);
        }
        if (config.LOG_COLOR)
        {
            Logging::setLoggingColor(true);
        }
    }

    bool consoleLogging =
        !logToFile || config.LOG_FILE_PATH.empty() || mConsoleLog;
    Logging::setLoggingToConsole(consoleLogging);
    Logging::setLogLevel(mLogLevel, nullptr);

    config.REPORT_METRICS = mMetrics;
    return config;
}

CommandLine::CommandLine(std::vector<Command> const& commands)
    : mCommands{commands}
{
    mCommands.push_back(Command{"help", "display list of available commands",
                                [this](CommandLineArgs const& args) {
                                    writeToStream(args.mExeName, std::cout);
                                    return 0;
                                }});

    std::sort(
        std::begin(mCommands), std::end(mCommands),
        [](Command const& x, Command const& y) { return x.name() < y.name(); });
}

CommandLine::AdjustedCommandLine
CommandLine::adjustCommandLine(clara::detail::Args const& args)
{
    auto tokens = clara::detail::TokenStream{args};
    auto command = std::string{};
    auto remainingTokens = std::vector<std::string>{};
    auto found = false;
    auto optionValue = false;

    while (tokens)
    {
        auto token = *tokens;
        if (found || optionValue)
        {
            remainingTokens.push_back(token.token);
            optionValue = false;
        }
        else if (token.type == clara::detail::TokenType::Argument)
        {
            command = token.token;
            found = true;
        }
        else // clara::detail::TokenType::Option
        {
            auto commonIt =
                std::find_if(std::begin(ConfigOption::COMMON_OPTIONS),
                             std::end(ConfigOption::COMMON_OPTIONS),
                             [&](ConfigOption::Common const& option) {
                                 return token.token == option.first;
                             });
            if (commonIt != std::end(ConfigOption::COMMON_OPTIONS))
            {
                remainingTokens.push_back(token.token);
                optionValue = commonIt->second;
            }
            else
            {
                return {}; // fallback to deprecated command line syntax
            }
        }
        ++tokens;
    }

    return CommandLine::AdjustedCommandLine{command, remainingTokens};
}

std::optional<CommandLine::Command>
CommandLine::selectCommand(std::string const& commandName)
{
    auto command = std::find_if(
        std::begin(mCommands), std::end(mCommands),
        [&](Command const& command) { return command.name() == commandName; });
    if (command != std::end(mCommands))
    {
        return std::make_optional<Command>(*command);
    }

    command = std::find_if(
        std::begin(mCommands), std::end(mCommands),
        [&](Command const& command) { return command.name() == "help"; });
    if (command != std::end(mCommands))
    {
        return std::make_optional<Command>(*command);
    }
    return std::nullopt;
}

void
CommandLine::writeToStream(std::string const& exeName, std::ostream& os) const
{
#ifdef BUILD_TESTS
    // Printing this line enables automatic test discovery in VSCode, and
    // it is generally harmless otherwise (only shown in a BUILD_TESTS build).
    std::cout << "Catch2 v" << CATCH_VERSION_MAJOR << "." << CATCH_VERSION_MINOR
              << "." << CATCH_VERSION_PATCH << std::endl;
#endif
    os << "usage:\n"
       << "  " << exeName << " "
       << "COMMAND";
    os << "\n\nwhere COMMAND is one of following:" << std::endl;

    size_t consoleWidth = CLARA_TEXTFLOW_CONFIG_CONSOLE_WIDTH;
    size_t commandWidth = 0;
    for (auto const& command : mCommands)
        commandWidth = std::max(commandWidth, command.name().size() + 2);

    commandWidth = std::min(commandWidth, consoleWidth / 2);

    for (auto const& command : mCommands)
    {
        auto row = clara::TextFlow::Column(command.name())
                       .width(commandWidth)
                       .indent(2) +
                   clara::TextFlow::Spacer(4) +
                   clara::TextFlow::Column(command.description())
                       .width(consoleWidth - 7 - commandWidth);
        os << row << std::endl;
    }
}
}

int
diagBucketStats(CommandLineArgs const& args)
{
    std::string bucketFile;
    bool aggAccountStats = false;

    return runWithHelp(
        args,
        {fileNameParser(bucketFile),
         clara::Opt{aggAccountStats}["--aggregate-account-stats"](
             "aggregate entries on a per account basis")},
        [&] {
            diagnostics::bucketStats(bucketFile, aggAccountStats);
            return 0;
        });
}

int
runReplayDebugMeta(CommandLineArgs const& args)
{
    // targetLedger=0 means replay all available tx meta
    uint32_t targetLedger = 0;
    CommandLine::ConfigOption configOption;
    std::string metaDir{"."};

    return runWithHelp(
        args,
        {configurationParser(configOption), historyLedgerNumber(targetLedger),
         metaDirParser(metaDir).required()},
        [&] {
            VirtualClock clock(VirtualClock::REAL_TIME);
            auto cfg = configOption.getConfig();

            cfg.setNoListen();
            cfg.AUTOMATIC_SELF_CHECK_PERIOD = std::chrono::seconds::zero();

            auto app = Application::create(clock, cfg, false);
            app->start();

            auto& wm = app->getWorkScheduler();

            std::filesystem::path dir(metaDir);

            auto catchupWork =
                wm.executeWork<ReplayDebugMetaWork>(targetLedger, dir);
            if (catchupWork->getState() == BasicWork::State::WORK_SUCCESS)
            {
                LOG_INFO(DEFAULT_LOG, "Replay finished");
                return 0;
            }
            else
            {
                LOG_INFO(DEFAULT_LOG, "Replay finished");
                return 1;
            }
        });
}

int
runCatchup(CommandLineArgs const& args)
{
    CommandLine::ConfigOption configOption;
    std::string catchupString;
    std::string outputFile;
    std::string archive;
    std::string trustedCheckpointHashesFile;
    bool completeValidation = false;
    bool inMemory = false;
    bool forceUntrusted = false;
    std::string hash;
    std::string stream;

    auto validateCatchupString = [&] {
        try
        {
            parseCatchup(catchupString, hash, completeValidation);
            return std::string{};
        }
        catch (std::runtime_error& e)
        {
            return std::string{e.what()};
        }
    };

    auto validateCatchupArchive = [&] {
        if (iequals(archive, "any") || archive.empty())
        {
            return std::string{};
        }

        auto config = configOption.getConfig();
        if (config.HISTORY.find(archive) != config.HISTORY.end())
        {
            return std::string{};
        }
        return std::string{"Catchup error: bad archive name"};
    };

    auto catchupStringParser = ParserWithValidation{
        clara::Arg(catchupString, "DESTINATION-LEDGER/LEDGER-COUNT").required(),
        validateCatchupString};
    auto trustedCheckpointHashesParser = [](std::string& file) {
        return clara::Opt{file, "FILE-NAME"}["--trusted-checkpoint-hashes"](
            "get destination ledger hash from trusted output of "
            "'verify-checkpoints'");
    };
    auto catchupArchiveParser = ParserWithValidation{
        historyArchiveParser(archive), validateCatchupArchive};
    auto disableBucketGC = false;

    auto validationParser = [](bool& completeValidation) {
        return clara::Opt{completeValidation}["--extra-verification"](
            "verify all files from the archive for the catchup range");
    };

    return runWithHelp(
        args,
        {configurationParser(configOption), catchupStringParser,
         catchupArchiveParser,
         trustedCheckpointHashesParser(trustedCheckpointHashesFile),
         outputFileParser(outputFile), disableBucketGCParser(disableBucketGC),
         validationParser(completeValidation), inMemoryParser(inMemory),
         ledgerHashParser(hash), ledgerHashParser(hash),
         forceUntrustedCatchup(forceUntrusted),
         metadataOutputStreamParser(stream)},
        [&] {
            auto config = configOption.getConfig();
            // Don't call config.setNoListen() here as we might want to
            // access the /info HTTP endpoint during catchup.
            config.RUN_STANDALONE = true;
            config.MANUAL_CLOSE = true;
            config.DISABLE_BUCKET_GC = disableBucketGC;

            if (config.AUTOMATIC_MAINTENANCE_PERIOD.count() > 0 &&
                config.AUTOMATIC_MAINTENANCE_COUNT > 0)
            {
                // If the user did not _disable_ maintenance, turn the dial up
                // to be much more aggressive about running maintenance during a
                // bulk catchup, otherwise the DB is likely to overflow with
                // unwanted history.
                config.AUTOMATIC_MAINTENANCE_PERIOD = std::chrono::seconds{30};
                config.AUTOMATIC_MAINTENANCE_COUNT = MAINTENANCE_LEDGER_COUNT;
            }

            maybeSetMetadataOutputStream(config, stream);

            VirtualClock clock(VirtualClock::REAL_TIME);
            int result;
            {
                auto app = Application::create(clock, config, inMemory);
                auto const& ham = app->getHistoryArchiveManager();
                auto archivePtr = ham.getHistoryArchive(archive);
                if (iequals(archive, "any"))
                {
                    archivePtr = ham.selectRandomReadableHistoryArchive();
                }

                CatchupConfiguration cc =
                    parseCatchup(catchupString, hash, completeValidation);

                if (!trustedCheckpointHashesFile.empty() && !hash.empty())
                {
                    throw std::runtime_error(
                        "Either --trusted-checkpoint-hashes or --trusted-hash "
                        "should be specified, but not both");
                }

                if (!trustedCheckpointHashesFile.empty())
                {
                    if (!HistoryManager::isLastLedgerInCheckpoint(
                            cc.toLedger(), app->getConfig()))
                    {
                        throw std::runtime_error(
                            "destination ledger is not a checkpoint boundary,"
                            " but trusted checkpoints file was provided");
                    }
                    Hash h = WriteVerifiedCheckpointHashesWork::
                        loadHashFromJsonOutput(cc.toLedger(),
                                               trustedCheckpointHashesFile);
                    if (isZero(h))
                    {
                        throw std::runtime_error("destination ledger not found "
                                                 "in trusted checkpoints file");
                    }
                    LedgerNumHashPair pair;
                    pair.first = cc.toLedger();
                    pair.second = std::make_optional<Hash>(h);
                    LOG_INFO(DEFAULT_LOG, "Found trusted hash {} for ledger {}",
                             hexAbbrev(h), cc.toLedger());
                    cc = CatchupConfiguration(pair, cc.count(), cc.mode());
                }

                if (hash.empty() && !forceUntrusted)
                {
                    CLOG_WARNING(
                        History,
                        "Unsafe command: use --trusted-checkpoint-hashes or "
                        "--trusted-hash to ensure catchup integrity. If you "
                        "want to run untrusted catchup, use "
                        "--force-untrusted-catchup.");
                }

                Json::Value catchupInfo;
                result = catchup(app, cc, catchupInfo, archivePtr);
                if (!catchupInfo.isNull())
                {
                    writeCatchupInfo(catchupInfo, outputFile);
                }
            }
            return result;
        });
}

int
runPublish(CommandLineArgs const& args)
{
    CommandLine::ConfigOption configOption;

    return runWithHelp(args, {configurationParser(configOption)}, [&] {
        auto config = configOption.getConfig();
        config.setNoListen();

        VirtualClock clock(VirtualClock::REAL_TIME);
        auto app = Application::create(clock, config, false);
        return publish(app);
    });
}

int
runWriteVerifiedCheckpointHashes(CommandLineArgs const& args)
{
    std::string outputFile;
    std::optional<std::string> trustedHashFile;
    uint32_t startLedger = 0;
    std::string startHash;
    std::optional<std::uint32_t> fromLedger;
    CommandLine::ConfigOption configOption;
    return runWithHelp(
        args,
        {configurationParser(configOption), historyLedgerNumber(startLedger),
         historyHashParser(startHash), fromLedgerNumberParser(fromLedger),
         trustedHashFileParser(trustedHashFile),
         outputFileParser(outputFile).required()},
        [&] {
            if (outputFile.empty())
            {
                LOG_ERROR(DEFAULT_LOG, "Must specify --output-file");
                return 1;
            }
            if (fromLedger && trustedHashFile)
            {
                LOG_ERROR(DEFAULT_LOG, "Cannot specify both --from-ledger and "
                                       "--trusted-hash-file");
                return 1;
            }
            std::optional<LedgerNumHashPair> latestTrustedHashPair;
            if (trustedHashFile)
            {
                // Pasrse the latest hash from the trusted hash file before
                // starting the application and connecting to the network so
                // that we can exit early if there is malformed input.
                latestTrustedHashPair.emplace(
                    WriteVerifiedCheckpointHashesWork::
                        loadLatestHashPairFromJsonOutput(*trustedHashFile));
            }
            VirtualClock clock(VirtualClock::REAL_TIME);
            auto cfg = configOption.getConfig();

            // Set up for quick no-catchup mode.
            cfg.QUORUM_INTERSECTION_CHECKER = false;
            cfg.MODE_DOES_CATCHUP = false;

            auto app = Application::create(clock, cfg, false);
            app->start();

            auto& io = clock.getIOContext();
            asio::io_context::work mainWork(io);
            LedgerNumHashPair authPair;

            while (!(io.stopped() || authPair.second))
            {
                clock.crank();
                setAuthenticatedLedgerHashPair(app, authPair, startLedger,
                                               startHash);
            }
            if (authPair.second)
            {
                app->getOverlayManager().shutdown();
                app->getHerder().shutdown();
                app->getWorkScheduler()
                    .executeWork<WriteVerifiedCheckpointHashesWork>(
                        authPair, outputFile, trustedHashFile,
                        latestTrustedHashPair, fromLedger);
                app->gracefulStop();
                return 0;
            }
            else
            {
                app->gracefulStop();
                return 1;
            }
        });
}

int
runConvertId(CommandLineArgs const& args)
{
    std::string id;

    return runWithHelp(args, {requiredArgParser(id, "ID")}, [&] {
        StrKeyUtils::logKey(std::cout, id);
        return 0;
    });
}

int
runDumpXDR(CommandLineArgs const& args)
{
    std::string xdr;
    bool compact = false;

    return runWithHelp(args, {compactParser(compact), fileNameParser(xdr)},
                       [&] {
                           dumpXdrStream(xdr, compact);
                           return 0;
                       });
}

int
runEncodeAsset(CommandLineArgs const& args)
{
    std::string code, issuer;

    return runWithHelp(args, {codeParser(code), issuerParser(issuer)}, [&] {
        Asset asset;
        if (code.empty() && issuer.empty())
        {
            asset.type(ASSET_TYPE_NATIVE);
        }
        else if (code.empty() || issuer.empty())
        {
            throw std::runtime_error("If one of code or issuer is defined, the "
                                     "other must be defined");
        }
        else if (code.size() <= 4)
        {
            asset.type(ASSET_TYPE_CREDIT_ALPHANUM4);
            strToAssetCode(asset.alphaNum4().assetCode, code);
            asset.alphaNum4().issuer = KeyUtils::fromStrKey<PublicKey>(issuer);
        }
        else if (code.size() <= 12)
        {
            asset.type(ASSET_TYPE_CREDIT_ALPHANUM12);
            strToAssetCode(asset.alphaNum12().assetCode, code);
            asset.alphaNum12().issuer = KeyUtils::fromStrKey<PublicKey>(issuer);
        }
        else
        {
            throw std::runtime_error("Asset code must be <= 12 characters");
        }
        std::cout << decoder::encode_b64(xdr::xdr_to_opaque(asset))
                  << std::endl;
        return 0;
    });
}

int
runForceSCP(CommandLineArgs const& args)
{
    CommandLine::ConfigOption configOption;
    auto reset = false;

    auto resetOption = clara::Opt{reset}["--reset"](
        "reset force SCP flag, so next time stellar-core "
        "is run it will wait to hear from the network "
        "rather than starting with the local ledger");

    return runWithHelp(args, {configurationParser(configOption), resetOption},
                       [&] {
                           setForceSCPFlag();
                           return 0;
                       });
}

int
runGenSeed(CommandLineArgs const& args)
{
    return runWithHelp(args, {}, [&] {
        genSeed();
        return 0;
    });
}

int
runHttpCommand(CommandLineArgs const& args)
{
    CommandLine::ConfigOption configOption;
    std::string command;

    return runWithHelp(args,
                       {configurationParser(configOption),
                        requiredArgParser(command, "COMMAND")},
                       [&] {
                           httpCommand(command,
                                       configOption.getConfig(false).HTTP_PORT);
                           return 0;
                       });
}

int
runSelfCheck(CommandLineArgs const& args)
{
    CommandLine::ConfigOption configOption;

    return runWithHelp(args, {configurationParser(configOption)},
                       [&] { return selfCheck(configOption.getConfig()); });
}

int
runMergeBucketList(CommandLineArgs const& args)
{
    CommandLine::ConfigOption configOption;
    std::string outputDir{"."};

    return runWithHelp(
        args,
        {configurationParser(configOption),
         outputDirParser(outputDir).required()},
        [&] { return mergeBucketList(configOption.getConfig(), outputDir); });
}

int
runDumpStateArchivalStatistics(CommandLineArgs const& args)
{
    CommandLine::ConfigOption configOption;
    return runWithHelp(args, {configurationParser(configOption)}, [&] {
        return dumpStateArchivalStatistics(configOption.getConfig());
    });
}

int
runDumpLedger(CommandLineArgs const& args)
{
    CommandLine::ConfigOption configOption;
    std::string outputFile;
    std::optional<std::string> filterQuery;
    std::optional<uint32_t> lastModifiedLedgerCount;
    std::optional<uint64_t> limit;
    std::optional<std::string> groupBy;
    std::optional<std::string> aggregate;
    bool includeAllStates = false;
    return runWithHelp(
        args,
        {configurationParser(configOption),
         outputFileParser(outputFile).required(),
         filterQueryParser(filterQuery),
         lastModifiedLedgerCountParser(lastModifiedLedgerCount),
         limitParser(limit), groupByParser(groupBy), aggregateParser(aggregate),
         includeAllStatesParser(includeAllStates)},
        [&] {
            return dumpLedger(configOption.getConfig(), outputFile, filterQuery,
                              lastModifiedLedgerCount, limit, groupBy,
                              aggregate, includeAllStates);
        });
}

int
runNewDB(CommandLineArgs const& args)
{
    CommandLine::ConfigOption configOption;
    [[maybe_unused]] bool minimalForInMemoryMode = false;

    auto minimalDBParser = [](bool& minimalForInMemoryMode) {
        return clara::Opt{
            minimalForInMemoryMode}["--minimal-for-in-memory-mode"](
            "(DEPRECATED) flag is ignored and will be removed soon.");
    };

    return runWithHelp(args,
                       {configurationParser(configOption),
                        minimalDBParser(minimalForInMemoryMode)},
                       [&] {
                           auto cfg = configOption.getConfig();
                           initializeDatabase(cfg);
                           return 0;
                       });
}

int
runUpgradeDB(CommandLineArgs const& args)
{
    CommandLine::ConfigOption configOption;

    return runWithHelp(args, {configurationParser(configOption)}, [&] {
        auto cfg = configOption.getConfig();
        VirtualClock clock;
        cfg.setNoListen();
        Application::create(clock, cfg, false);
        return 0;
    });
}

int
getSettingsUpgradeTransactions(CommandLineArgs const& args)
{
    std::string netId;

    int64_t seqNum;
    std::string upgradeFile;

    bool signTxs = false;

    auto signTxnOption =
        clara::Opt{signTxs}["--signtxs"]("sign all transactions");

    auto netIdOption = clara::Opt(netId, "NETWORK-PASSPHRASE")["--netid"](
                           "network ID used for signing")
                           .required();

    auto netIdParser = ParserWithValidation{
        netIdOption, required(netId, "NETWORK-PASSPHRASE")};

    std::string id;

    std::string base64Xdr;
    auto base64Option = clara::Opt{base64Xdr, "XDR-BASE64"}["--xdr"](
                            "ConfigUpgradeSet in base64")
                            .required();

    auto base64Parser =
        ParserWithValidation{base64Option, required(base64Xdr, "XDR-BASE64")};

    ParserWithValidation seqNumParser{
        clara::Arg(seqNum, "SequenceNumber").required(),
        [&] { return seqNum >= 0 ? "" : "SequenceNumber must be >= 0"; }};

    return runWithHelp(
        args,
        {requiredArgParser(id, "PublicKey"), seqNumParser,
         requiredArgParser(netId, "NetworkPassphrase"), base64Parser,
         signTxnOption},
        [&] {
            ConfigUpgradeSet upgradeSet;
            std::vector<uint8_t> binBlob;
            decoder::decode_b64(base64Xdr, binBlob);
            xdr::xdr_from_opaque(binBlob, upgradeSet);

            PublicKey pk = KeyUtils::fromStrKey<PublicKey>(id);

            std::vector<TransactionEnvelope> txsToSign;
            auto restoreRes = getWasmRestoreTx(pk, seqNum + 1);
            txsToSign.emplace_back(restoreRes.first);

            auto uploadRes = getUploadTx(pk, seqNum + 2);
            txsToSign.emplace_back(uploadRes.first);
            auto const& contractCodeLedgerKey = uploadRes.second;

            auto createRes =
                getCreateTx(pk, contractCodeLedgerKey, netId, seqNum + 3);
            txsToSign.emplace_back(std::get<0>(createRes));
            auto const& contractSourceRefLedgerKey = std::get<1>(createRes);
            auto const& contractID = std::get<2>(createRes);

            auto invokeRes = getInvokeTx(pk, contractCodeLedgerKey,
                                         contractSourceRefLedgerKey, contractID,
                                         upgradeSet, seqNum + 4);
            txsToSign.emplace_back(invokeRes.first);
            auto const& upgradeSetKey = invokeRes.second;

            if (signTxs)
            {
                signtxns(txsToSign, netId, true, false, true);
            }
            else
            {
                TransactionSignaturePayload payload;
                payload.networkId = sha256(netId);
                payload.taggedTransaction.type(ENVELOPE_TYPE_TX);

                auto tx1 =
                    decoder::encode_b64(xdr::xdr_to_opaque(txsToSign.at(0)));
                auto payload1 = payload;
                payload1.taggedTransaction.tx() = txsToSign.at(0).v1().tx;

                auto tx2 =
                    decoder::encode_b64(xdr::xdr_to_opaque(txsToSign.at(1)));
                auto payload2 = payload;
                payload2.taggedTransaction.tx() = txsToSign.at(1).v1().tx;

                auto tx3 =
                    decoder::encode_b64(xdr::xdr_to_opaque(txsToSign.at(2)));
                auto payload3 = payload;
                payload3.taggedTransaction.tx() = txsToSign.at(2).v1().tx;

                std::cerr
                    << "Unsigned TransactionEnvelope to upload upgrade WASM "
                    << std::endl;
                std::cout << tx1 << std::endl;
                std::cout << binToHex(xdr::xdr_to_opaque(
                                 sha256(xdr::xdr_to_opaque(payload1))))
                          << std::endl;

                std::cerr << "Unsigned TransactionEnvelope to create upgrade "
                             "contract "
                          << std::endl;

                std::cout << tx2 << std::endl;
                std::cout << binToHex(xdr::xdr_to_opaque(
                                 sha256(xdr::xdr_to_opaque(payload2))))
                          << std::endl;

                std::cerr
                    << "Unsigned TransactionEnvelope to invoke contract with "
                       "upgrade bytes "
                    << std::endl;
                std::cout << tx3 << std::endl;
                std::cout << binToHex(xdr::xdr_to_opaque(
                                 sha256(xdr::xdr_to_opaque(payload3))))
                          << std::endl;
            }

            std::cout << decoder::encode_b64(xdr::xdr_to_opaque(upgradeSetKey))
                      << std::endl;

            return 0;
        });
}

int
runPrintPublishQueue(CommandLineArgs const& args)
{
    CommandLine::ConfigOption configOption;

    return runWithHelp(args, {configurationParser(configOption)}, [&] {
        auto cfg = configOption.getConfig();
        VirtualClock clock(VirtualClock::REAL_TIME);
        cfg.setNoListen();
        Application::pointer app = Application::create(clock, cfg, false);
        cereal::JSONOutputArchive archive(std::cout);
        archive.makeArray();
        for (auto const& has :
             HistoryManager::getPublishQueueStates(app->getConfig()))
        {
            has.serialize(archive);
        }
        return 0;
    });
}

int
runCheckQuorumIntersection(CommandLineArgs const& args)
{
    CommandLine::ConfigOption configOption;
    std::string jsonPath;
    return runWithHelp(
        args,
        {logLevelParser(configOption.mLogLevel), fileNameParser(jsonPath),
         consoleParser(configOption.mConsoleLog),
         clara::Opt{configOption.mConfigFile,
                    "FILE-NAME"}["--conf"](fmt::format(
             FMT_STRING("specify a config file to enable human readable "
                        "node names (optional, '{}' for STDIN)"),
             Config::STDIN_SPECIAL_NAME))},
        [&] {
            try
            {
                std::optional<Config> cfg = std::nullopt;
                if (configOption.mConfigFile.empty())
                {
                    // Need to set up logging in this case because there is no
                    // `getConfig` call (which would otherwise set up logging)
                    Logging::setLoggingToConsole(true);
                    Logging::setLogLevel(configOption.mLogLevel, nullptr);
                }
                else
                {
                    cfg.emplace(configOption.getConfig(true));
                }
                if (checkQuorumIntersectionFromJson(jsonPath, cfg))
                {
                    CLOG_INFO(SCP, "Network enjoys quorum intersection");
                    return 0;
                }
                else
                {
                    CLOG_WARNING(SCP,
                                 "Network does not enjoy quorum intersection");
                    return 1;
                }
            }
            catch (KeyUtils::InvalidStrKey const& e)
            {
                CLOG_FATAL(
                    SCP,
                    "check-quorum-intersection encountered an "
                    "error: Invalid public key in JSON file. JSON file must be "
                    "generated with the 'fullkeys' parameter set to 'true'.");
                return 2;
            }
            catch (std::exception const& e)
            {
                CLOG_FATAL(SCP,
                           "check-quorum-intersection encountered an error: {}",
                           e.what());
                return 2;
            }
        });
}

int
runNewHist(CommandLineArgs const& args)
{
    CommandLine::ConfigOption configOption;
    std::vector<std::string> newHistories;

    return runWithHelp(args,
                       {configurationParser(configOption),
                        requiredArgParser(newHistories, "HISTORY-LABEL")},
                       [&] {
                           return initializeHistories(configOption.getConfig(),
                                                      newHistories);
                       });
}

int
runOfflineInfo(CommandLineArgs const& args)
{
    CommandLine::ConfigOption configOption;

    return runWithHelp(args, {configurationParser(configOption)}, [&] {
        showOfflineInfo(configOption.getConfig(), true);
        return 0;
    });
}

int
runPrintXdr(CommandLineArgs const& args)
{
    std::string xdr;
    std::string fileType{"auto"};
    auto base64 = false;
    auto compact = false;
    auto rawMode = false;

    auto fileTypeOpt = clara::Opt(fileType, "FILE-TYPE")["--filetype"](
        "[auto|asset|ledgerentry|ledgerheader|meta|result|resultpair|tx|"
        "txfee]");

    return runWithHelp(args,
                       {fileNameParser(xdr), fileTypeOpt, base64Parser(base64),
                        compactParser(compact),
                        clara::Opt{rawMode}["--raw"](
                            "raw mode, do not normalize some objects")},
                       [&] {
                           printXdr(xdr, fileType, base64, compact, rawMode);
                           return 0;
                       });
}

int
runReportLastHistoryCheckpoint(CommandLineArgs const& args)
{
    CommandLine::ConfigOption configOption;
    std::string outputFile;

    return runWithHelp(
        args, {configurationParser(configOption), outputFileParser(outputFile)},
        [&] {
            reportLastHistoryCheckpoint(configOption.getConfig(), outputFile);
            return 0;
        });
}

int
run(CommandLineArgs const& args)
{
    CommandLine::ConfigOption configOption;
    auto disableBucketGC = false;
    std::string stream;
    bool waitForConsensus = false;
    [[maybe_unused]] bool inMemory = false;
    [[maybe_unused]] uint32_t startAtLedger = 0;
    [[maybe_unused]] std::string startAtHash;

    return runWithHelp(
        args,
        {configurationParser(configOption),
         disableBucketGCParser(disableBucketGC),
         metadataOutputStreamParser(stream), inMemoryParser(inMemory),
         waitForConsensusParser(waitForConsensus),
         startAtLedgerParser(startAtLedger), startAtHashParser(startAtHash)},
        [&] {
            Config cfg;
            std::shared_ptr<VirtualClock> clock;
            VirtualClock::Mode clockMode = VirtualClock::REAL_TIME;
            Application::pointer app;

            try
            {
                // First, craft and validate the configuration
                cfg = configOption.getConfig();
                cfg.DISABLE_BUCKET_GC = disableBucketGC;

                if (!cfg.OP_APPLY_SLEEP_TIME_DURATION_FOR_TESTING.empty() ||
                    !cfg.OP_APPLY_SLEEP_TIME_WEIGHT_FOR_TESTING.empty())
                {
                    cfg.DATABASE = SecretValue{"sqlite3://:memory:"};
                    cfg.MODE_STORES_HISTORY_MISC = false;
                    cfg.MODE_ENABLES_BUCKETLIST = false;
                    cfg.PREFETCH_BATCH_SIZE = 0;
                }

                maybeSetMetadataOutputStream(cfg, stream);
                cfg.FORCE_SCP =
                    cfg.NODE_IS_VALIDATOR ? !waitForConsensus : false;

                if (cfg.MANUAL_CLOSE)
                {
                    if (!cfg.NODE_IS_VALIDATOR)
                    {
                        LOG_ERROR(DEFAULT_LOG, "Starting stellar-core in "
                                               "MANUAL_CLOSE mode requires "
                                               "NODE_IS_VALIDATOR to be set");
                        return 1;
                    }
                    if (cfg.RUN_STANDALONE)
                    {
                        clockMode = VirtualClock::VIRTUAL_TIME;
                        if (cfg.AUTOMATIC_MAINTENANCE_COUNT != 0 ||
                            cfg.AUTOMATIC_MAINTENANCE_PERIOD.count() != 0)
                        {
                            LOG_WARNING(DEFAULT_LOG,
                                        "Using MANUAL_CLOSE and RUN_STANDALONE "
                                        "together induces virtual time, which "
                                        "requires automatic maintenance to be "
                                        "disabled. AUTOMATIC_MAINTENANCE_COUNT "
                                        "and AUTOMATIC_MAINTENANCE_PERIOD are "
                                        "being overridden to 0.");
                            cfg.AUTOMATIC_MAINTENANCE_COUNT = 0;
                            cfg.AUTOMATIC_MAINTENANCE_PERIOD =
                                std::chrono::seconds{0};
                        }
                    }
                }

                if (cfg.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING)
                {
                    LOG_WARNING(DEFAULT_LOG, "Artificial acceleration of time "
                                             "enabled (for testing only)");
                }

                // Second, setup the app with the final configuration.
                clock = std::make_shared<VirtualClock>(clockMode);
                app = setupApp(cfg, *clock);
                if (!app)
                {
                    LOG_ERROR(DEFAULT_LOG,
                              "Unable to setup the application to run");
                    return 1;
                }
            }
            catch (std::exception& e)
            {
                LOG_FATAL(DEFAULT_LOG, "Got an exception: {}", e.what());
                LOG_FATAL(DEFAULT_LOG, "{}", REPORT_INTERNAL_BUG);
                return 1;
            }

            // Finally, run the application outside of catch block so that we
            // properly capture crashes
            return runApp(app);
        });
}

int
runSecToPub(CommandLineArgs const& args)
{
    return runWithHelp(args, {}, [&] {
        priv2pub();
        return 0;
    });
}

int
runSignTransaction(CommandLineArgs const& args)
{
    std::string transaction;
    std::string netId;
    bool base64;

    auto netIdOption = clara::Opt(netId, "NETWORK-PASSPHRASE")["--netid"](
                           "network ID used for signing")
                           .required();
    auto netIdParser = ParserWithValidation{
        netIdOption, required(netId, "NETWORK-PASSPHRASE")};

    return runWithHelp(
        args, {fileNameParser(transaction), netIdParser, base64Parser(base64)},
        [&] {
            signtxn(transaction, netId, base64);
            return 0;
        });
}

int
runVersion(CommandLineArgs const&)
{
    rust::Vec<SorobanVersionInfo> rustVersions =
        rust_bridge::get_soroban_version_info(
            Config::CURRENT_LEDGER_PROTOCOL_VERSION);

    std::cout << STELLAR_CORE_VERSION << std::endl;
    std::cout << "ledger protocol version: "
              << Config::CURRENT_LEDGER_PROTOCOL_VERSION << std::endl;
    std::cout << "rust version: " << rust_bridge::get_rustc_version().c_str()
              << std::endl;

    std::cout << "soroban-env-host versions: " << std::endl;

    size_t i = 0;
    for (auto& host : rustVersions)
    {
        std::cout << "    host[" << i << "]:" << std::endl;
        std::cout << "        package version: " << host.env_pkg_ver.c_str()
                  << std::endl;

        std::cout << "        git version: " << host.env_git_rev.c_str()
                  << std::endl;

        std::cout << "        ledger protocol version: " << host.env_max_proto
                  << std::endl;

        std::cout << "        pre-release version: " << host.env_pre_release_ver
                  << std::endl;

        std::cout << "        rs-stellar-xdr:" << std::endl;

        std::cout << "            package version: " << host.xdr_pkg_ver.c_str()
                  << std::endl;
        std::cout << "            git version: " << host.xdr_git_rev.c_str()
                  << std::endl;
        std::cout << "            base XDR git version: "
                  << host.xdr_base_git_rev.c_str() << std::endl;
        ++i;
    }
    return 0;
}

#ifdef BUILD_TESTS
int
runLoadXDR(CommandLineArgs const& args)
{
    CommandLine::ConfigOption configOption;
    std::string xdr;

    return runWithHelp(
        args, {configurationParser(configOption), fileNameParser(xdr)}, [&] {
            loadXdr(configOption.getConfig(), xdr);
            return 0;
        });
}

int
runRebuildLedgerFromBuckets(CommandLineArgs const& args)
{
    CommandLine::ConfigOption configOption;

    return runWithHelp(args, {configurationParser(configOption)}, [&] {
        rebuildLedgerFromBuckets(configOption.getConfig());
        return 0;
    });
}

ParserWithValidation
fuzzerModeParser(std::string& fuzzerModeArg, FuzzerMode& fuzzerMode)
{
    auto validateFuzzerMode = [&] {
        if (iequals(fuzzerModeArg, "overlay"))
        {
            fuzzerMode = FuzzerMode::OVERLAY;
            return "";
        }

        if (iequals(fuzzerModeArg, "tx"))
        {
            fuzzerMode = FuzzerMode::TRANSACTION;
            return "";
        }

        return "Unrecognized fuzz mode. Please select a valid mode.";
    };

    return {clara::Opt{fuzzerModeArg, "FUZZER-MODE"}["--mode"](
                "set the fuzzer mode. Expected modes: overlay, "
                "tx. Defaults to overlay."),
            validateFuzzerMode};
}

int
runFuzz(CommandLineArgs const& args)
{
    LogLevel logLevel{LogLevel::LVL_FATAL};
    std::vector<std::string> metrics;
    std::string fileName;
    std::string outputFile;
    int processID = 0;
    bool consoleLog = false;
    FuzzerMode fuzzerMode{FuzzerMode::OVERLAY};
    std::string fuzzerModeArg = "overlay";

    return runWithHelp(args,
                       {logLevelParser(logLevel), metricsParser(metrics),
                        consoleParser(consoleLog), fileNameParser(fileName),
                        outputFileParser(outputFile),
                        processIDParser(processID),
                        fuzzerModeParser(fuzzerModeArg, fuzzerMode)},
                       [&] {
                           Logging::setLogLevel(logLevel, nullptr);
                           if (!outputFile.empty())
                           {
                               Logging::setLoggingToFile(outputFile);
                           }

                           fuzz(fileName, metrics, processID, fuzzerMode);
                           return 0;
                       });
}

int
runGenFuzz(CommandLineArgs const& args)
{
    LogLevel logLevel{LogLevel::LVL_FATAL};
    std::string fileName;
    std::string outputFile;
    FuzzerMode fuzzerMode{FuzzerMode::OVERLAY};
    std::string fuzzerModeArg = "overlay";
    int processID = 0;

    return runWithHelp(
        args,
        {logLevelParser(logLevel), fileNameParser(fileName),
         outputFileParser(outputFile),
         fuzzerModeParser(fuzzerModeArg, fuzzerMode)},
        [&] {
            Logging::setLogLevel(logLevel, nullptr);
            if (!outputFile.empty())
            {
                Logging::setLoggingToFile(outputFile);
            }

            FuzzUtils::createFuzzer(processID, fuzzerMode)->genFuzz(fileName);
            return 0;
        });
}

int
runApplyLoad(CommandLineArgs const& args)
{
    CommandLine::ConfigOption configOption;

    return runWithHelp(args, {configurationParser(configOption)}, [&] {
        auto config = configOption.getConfig();
        config.RUN_STANDALONE = true;
        config.MANUAL_CLOSE = true;
        config.USE_CONFIG_FOR_GENESIS = true;
        config.TESTING_UPGRADE_MAX_TX_SET_SIZE = 1000;
        config.LEDGER_PROTOCOL_VERSION =
            Config::CURRENT_LEDGER_PROTOCOL_VERSION;

        TmpDirManager tdm(std::string("soroban-storage-meta-"));
        TmpDir td = tdm.tmpDir("soroban-meta-ok");
        std::string metaPath = td.getName() + "/stream.xdr";

        config.METADATA_OUTPUT_STREAM = metaPath;

        VirtualClock clock(VirtualClock::REAL_TIME);
        auto appPtr = Application::create(clock, config);

        auto& app = *appPtr;
        {
            app.start();

            ApplyLoad al(app);

            auto& ledgerClose =
                app.getMetrics().NewTimer({"ledger", "ledger", "close"});
            ledgerClose.Clear();

            auto& cpuInsRatio = app.getMetrics().NewHistogram(
                {"soroban", "host-fn-op", "invoke-time-fsecs-cpu-insn-ratio"});
            cpuInsRatio.Clear();

            auto& cpuInsRatioExclVm = app.getMetrics().NewHistogram(
                {"soroban", "host-fn-op",
                 "invoke-time-fsecs-cpu-insn-ratio-excl-vm"});
            cpuInsRatioExclVm.Clear();

            auto& ledgerCpuInsRatio = app.getMetrics().NewHistogram(
                {"soroban", "host-fn-op", "ledger-cpu-insns-ratio"});
            ledgerCpuInsRatio.Clear();

            auto& ledgerCpuInsRatioExclVm = app.getMetrics().NewHistogram(
                {"soroban", "host-fn-op", "ledger-cpu-insns-ratio-excl-vm"});
            ledgerCpuInsRatioExclVm.Clear();

            for (size_t i = 0; i < 100; ++i)
            {
                app.getBucketManager().getLiveBucketList().resolveAllFutures();
                releaseAssert(app.getBucketManager()
                                  .getLiveBucketList()
                                  .futuresAllResolved());
                al.benchmark();
            }

            CLOG_INFO(Perf, "Max ledger close: {} milliseconds",
                      ledgerClose.max());
            CLOG_INFO(Perf, "Min ledger close: {} milliseconds",
                      ledgerClose.min());
            CLOG_INFO(Perf, "Mean ledger close:  {} milliseconds",
                      ledgerClose.mean());
            CLOG_INFO(Perf, "stddev ledger close:  {} milliseconds",
                      ledgerClose.std_dev());

            CLOG_INFO(Perf, "Max CPU ins ratio: {}",
                      cpuInsRatio.max() / 1000000);
            CLOG_INFO(Perf, "Mean CPU ins ratio:  {}",
                      cpuInsRatio.mean() / 1000000);

            CLOG_INFO(Perf, "Max CPU ins ratio excl VM: {}",
                      cpuInsRatioExclVm.max() / 1000000);
            CLOG_INFO(Perf, "Mean CPU ins ratio excl VM:  {}",
                      cpuInsRatioExclVm.mean() / 1000000);
            CLOG_INFO(Perf, "stddev CPU ins ratio excl VM:  {}",
                      cpuInsRatioExclVm.std_dev() / 1000000);

            CLOG_INFO(Perf, "Ledger Max CPU ins ratio: {}",
                      ledgerCpuInsRatio.max() / 1000000);
            CLOG_INFO(Perf, "Ledger Mean CPU ins ratio:  {}",
                      ledgerCpuInsRatio.mean() / 1000000);
            CLOG_INFO(Perf, "Ledger stddev CPU ins ratio:  {}",
                      ledgerCpuInsRatio.std_dev() / 1000000);

            CLOG_INFO(Perf, "Ledger Max CPU ins ratio excl VM: {}",
                      ledgerCpuInsRatioExclVm.max() / 1000000);
            CLOG_INFO(Perf, "Ledger Mean CPU ins ratio excl VM:  {}",
                      ledgerCpuInsRatioExclVm.mean() / 1000000);
            CLOG_INFO(Perf,
                      "Ledger stddev CPU ins ratio excl VM:  {} milliseconds",
                      ledgerCpuInsRatioExclVm.std_dev() / 1000000);

            CLOG_INFO(Perf, "Tx count utilization {}%",
                      al.getTxCountUtilization().mean() / 1000.0);
            CLOG_INFO(Perf, "Instruction utilization {}%",
                      al.getInstructionUtilization().mean() / 1000.0);
            CLOG_INFO(Perf, "Tx size utilization {}%",
                      al.getTxSizeUtilization().mean() / 1000.0);
            CLOG_INFO(Perf, "Read bytes utilization {}%",
                      al.getReadByteUtilization().mean() / 1000.0);
            CLOG_INFO(Perf, "Write bytes utilization {}%",
                      al.getWriteByteUtilization().mean() / 1000.0);
            CLOG_INFO(Perf, "Read entry utilization {}%",
                      al.getReadEntryUtilization().mean() / 1000.0);
            CLOG_INFO(Perf, "Write entry utilization {}%",
                      al.getWriteEntryUtilization().mean() / 1000.0);

            CLOG_INFO(Perf, "Tx Success Rate: {:f}%", al.successRate() * 100);
        }

        return 0;
    });
}
#endif

int
handleCommandLine(int argc, char* const* argv)
{
    auto commandLine = CommandLine{
        {{"catchup",
          "execute catchup from history archives without connecting to "
          "network",
          runCatchup},
         {"replay-debug-meta", "apply ledgers from local debug metadata files",
          runReplayDebugMeta},
         {"verify-checkpoints", "write verified checkpoint ledger hashes",
          runWriteVerifiedCheckpointHashes},
         {"convert-id", "displays ID in all known forms", runConvertId},
         {"diag-bucket-stats", "reports statistics on the content of a bucket",
          diagBucketStats},
         {"dump-ledger", "dumps the current ledger state as JSON for debugging",
          runDumpLedger},
         {"dump-xdr", "dump an XDR file, for debugging", runDumpXDR},
         {"encode-asset", "Print an encoded asset in base 64 for debugging",
          runEncodeAsset},
         {"force-scp", "deprecated, use --wait-for-consensus option instead",
          runForceSCP},
         {"gen-seed", "generate and print a random node seed", runGenSeed},
         {"http-command", "send a command to local stellar-core",
          runHttpCommand},
         {"self-check", "performs diagnostic checks", runSelfCheck},
         {"merge-bucketlist", "writes diagnostic merged bucket list",
          runMergeBucketList},
         {"dump-archival-stats",
          "prints statistics about expired/evicted entries in the BucketList",
          runDumpStateArchivalStatistics},
         {"new-db", "creates or restores the DB to the genesis ledger",
          runNewDB},
         {"new-hist", "initialize history archives", runNewHist},
         {"offline-info", "return information for an offline instance",
          runOfflineInfo},
         {"print-xdr", "pretty-print one XDR envelope, then quit", runPrintXdr},
         {"publish",
          "execute publish of all items remaining in publish queue without "
          "connecting to network, may not publish last checkpoint if last "
          "closed ledger is on checkpoint boundary",
          runPublish},
         {"report-last-history-checkpoint",
          "report information about last checkpoint available in "
          "history archives",
          runReportLastHistoryCheckpoint},
         {"run", "run stellar-core node", run},
         {"sec-to-pub", "print the public key corresponding to a secret key",
          runSecToPub},
         {"sign-transaction",
          "add signature to transaction envelope, then quit",
          runSignTransaction},
         {"upgrade-db", "upgrade database schema to current version",
          runUpgradeDB},
         {"get-settings-upgrade-txs",
          "returns all transactions that need to be submitted to do a settings "
          "upgrade",
          getSettingsUpgradeTransactions},
         {"check-quorum-intersection",
          "check that a given network specified as a JSON file enjoys a quorum "
          "intersection",
          runCheckQuorumIntersection},
         {"print-publish-queue", "print all checkpoints scheduled for publish",
          runPrintPublishQueue},
#ifdef BUILD_TESTS
         {"load-xdr", "load an XDR bucket file, for testing", runLoadXDR},
         {"rebuild-ledger-from-buckets",
          "rebuild the current database ledger from the bucket list",
          runRebuildLedgerFromBuckets},
         {"fuzz", "run a single fuzz input and exit", runFuzz},
         {"gen-fuzz", "generate a random fuzzer input file", runGenFuzz},
         {"test", "execute test suite", runTest},
         {"apply-load", "run apply time load test", runApplyLoad},
#endif
         {"version", "print version information", runVersion}}};

    auto adjustedCommandLine = commandLine.adjustCommandLine({argc, argv});
    auto command = commandLine.selectCommand(adjustedCommandLine.first);
    bool didDefaultToHelp = command->name() != adjustedCommandLine.first;

    auto exeName = "stellar-core";
    auto commandName =
        fmt::format(FMT_STRING("{0} {1}"), exeName, command->name());
    auto args = CommandLineArgs{exeName, commandName, command->description(),
                                adjustedCommandLine.second};
    if (command->name() == "run" || command->name() == "fuzz")
    {
        // run outside of catch block so that we properly capture crashes
        return command->run(args);
    }

    try
    {
        int res = command->run(args);
        return didDefaultToHelp ? 1 : res;
    }
    catch (std::exception& e)
    {
        LOG_FATAL(DEFAULT_LOG, "Got an exception: {}", e.what());
        LOG_FATAL(DEFAULT_LOG, "{}", REPORT_INTERNAL_BUG);
        return 1;
    }
}
}
