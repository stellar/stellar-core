// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/CommandLine.h"
#include "bucket/BucketManager.h"
#include "catchup/CatchupConfiguration.h"
#include "catchup/CatchupRange.h"
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
#include "main/StellarCoreVersion.h"
#include "main/dumpxdr.h"
#include "overlay/OverlayManager.h"
#include "scp/QuorumSetUtils.h"
#include "src/catchup/simulation/TxSimApplyTransactionsWork.h"
#include "src/transactions/simulation/TxSimScaleBucketlistWork.h"
#include "util/Logging.h"
#include "util/types.h"
#include "work/WorkScheduler.h"

#ifdef BUILD_TESTS
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
                                              {"--help", false}};

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
outputDirParser(std::string& string)
{
    return clara::Opt{string, "DIR-NAME"}["--output-dir"]("output dir");
}

clara::Opt
logLevelParser(LogLevel& value)
{
    return clara::Opt{
        [&](std::string const& arg) { value = Logging::getLLfromString(arg); },
        "LEVEL"}["--ll"]("set the log level");
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

void
maybeEnableInMemoryMode(Config& config, bool inMemory, uint32_t startAtLedger,
                        std::string const& startAtHash, bool persistMinimalData)
{
    // First, ensure user parameters are valid
    if (!inMemory)
    {
        if (startAtLedger != 0)
        {
            throw std::runtime_error("--start-at-ledger requires --in-memory");
        }
        if (!startAtHash.empty())
        {
            throw std::runtime_error("--start-at-hash requires --in-memory");
        }
        return;
    }
    if (startAtLedger != 0 && startAtHash.empty())
    {
        throw std::runtime_error("--start-at-ledger requires --start-at-hash");
    }
    else if (startAtLedger == 0 && !startAtHash.empty())
    {
        throw std::runtime_error("--start-at-hash requires --start-at-ledger");
    }

    // Adjust configs for live in-memory-replay mode
    config.setInMemoryMode();

    if (startAtLedger != 0 && !startAtHash.empty())
    {
        config.MODE_AUTO_STARTS_OVERLAY = false;
    }

    // Set database to a small sqlite database used to store minimal data needed
    // to restore the ledger state
    if (persistMinimalData)
    {
        config.DATABASE = SecretValue{minimalDBForInMemoryMode(config)};
        config.MODE_STORES_HISTORY_LEDGERHEADERS = true;
        // Since this mode stores historical data (needed to restore
        // ledger state in certain scenarios), set maintenance to run
        // aggressively so that we only store a few ledgers worth of data
        config.AUTOMATIC_MAINTENANCE_PERIOD = std::chrono::seconds(30);
        config.AUTOMATIC_MAINTENANCE_COUNT = MAINTENANCE_LEDGER_COUNT;
    }
}

clara::Opt
inMemoryParser(bool& inMemory)
{
    return clara::Opt{inMemory}["--in-memory"](
        "store working ledger in memory rather than database");
};

clara::Opt
startAtLedgerParser(uint32_t& startAtLedger)
{
    return clara::Opt{startAtLedger, "LEDGER"}["--start-at-ledger"](
        "start in-memory run with replay from historical ledger number");
};

clara::Opt
startAtHashParser(std::string& startAtHash)
{
    return clara::Opt{startAtHash, "HASH"}["--start-at-hash"](
        "start in-memory run with replay from historical ledger hash");
};

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
parseCatchup(std::string const& catchup, bool extraValidation)
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
        return {parseLedger(catchup.substr(0, separatorIndex)),
                parseLedgerCount(catchup.substr(separatorIndex + 1)), mode};
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

    LOG_INFO(DEFAULT_LOG, "Config from {}", configFile);

    // yes you really have to do this 3 times
    Logging::setLogLevel(mLogLevel, nullptr);
    config.load(configFile);

    Logging::setFmt(KeyUtils::toShortString(config.NODE_SEED.getPublicKey()));
    Logging::setLogLevel(mLogLevel, nullptr);

    if (logToFile)
    {
        if (config.LOG_FILE_PATH.size())
            Logging::setLoggingToFile(config.LOG_FILE_PATH);
        if (config.LOG_COLOR)
            Logging::setLoggingColor(true);
        Logging::setLogLevel(mLogLevel, nullptr);
    }

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
runCatchup(CommandLineArgs const& args)
{
    CommandLine::ConfigOption configOption;
    std::string catchupString;
    std::string outputFile;
    std::string archive;
    std::string trustedCheckpointHashesFile;
    bool completeValidation = false;
    bool inMemory = false;
    bool forceBack = false;
    uint32_t startAtLedger = 0;
    std::string startAtHash;
    std::string stream;

    auto validateCatchupString = [&] {
        try
        {
            parseCatchup(catchupString, completeValidation);
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

    auto forceBackParser = [](bool& forceBackClean) {
        return clara::Opt{forceBackClean}["--force-back"](
            "force ledger state to a previous state, preserving older "
            "historical data");
    };

    return runWithHelp(
        args,
        {configurationParser(configOption), catchupStringParser,
         catchupArchiveParser,
         trustedCheckpointHashesParser(trustedCheckpointHashesFile),
         outputFileParser(outputFile), disableBucketGCParser(disableBucketGC),
         validationParser(completeValidation), inMemoryParser(inMemory),
         startAtLedgerParser(startAtLedger), startAtHashParser(startAtHash),
         metadataOutputStreamParser(stream), forceBackParser(forceBack)},
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

            maybeEnableInMemoryMode(config, inMemory, startAtLedger,
                                    startAtHash,
                                    /* persistMinimalData */ false);
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
                    parseCatchup(catchupString, completeValidation);
                if (!trustedCheckpointHashesFile.empty())
                {
                    auto const& hm = app->getHistoryManager();
                    if (!hm.isLastLedgerInCheckpoint(cc.toLedger()))
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

                if (forceBack)
                {
                    CatchupRange range(LedgerManager::GENESIS_LEDGER_SEQ, cc,
                                       app->getHistoryManager());
                    LOG_INFO(DEFAULT_LOG, "Force applying range {}-{}",
                             range.first(), range.last());
                    if (!range.applyBuckets())
                    {
                        throw std::runtime_error(
                            "force can only be used when buckets get applied");
                    }
                    // by dropping persistent state, we ensure that we don't
                    // leave the database in some half-reset state until
                    // startNewLedger completes later on
                    {
                        auto& ps = app->getPersistentState();
                        ps.setState(PersistentState::kLastClosedLedger, "");
                        ps.setState(PersistentState::kHistoryArchiveState, "");
                        ps.setState(PersistentState::kLastSCPData, "");
                        ps.setState(PersistentState::kLedgerUpgrades, "");
                    }

                    LOG_INFO(
                        DEFAULT_LOG,
                        "Cleaning historical data (this may take a while)");
                    auto& lm = app->getLedgerManager();
                    lm.deleteNewerEntries(app->getDatabase(), range.first());
                    // checkpoints
                    app->getHistoryManager().deleteCheckpointsNewerThan(
                        range.first());

                    // need to delete genesis ledger data (so that we can reset
                    // to it)
                    lm.deleteOldEntries(app->getDatabase(),
                                        LedgerManager::GENESIS_LEDGER_SEQ, 1);
                    LOG_INFO(
                        DEFAULT_LOG,
                        "Resetting ledger state to genesis before catching up");
                    app->resetLedgerState();
                    lm.startNewLedger();
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
    uint32_t startLedger = 0;
    std::string startHash;
    CommandLine::ConfigOption configOption;
    return runWithHelp(
        args,
        {configurationParser(configOption), historyLedgerNumber(startLedger),
         historyHashParser(startHash), outputFileParser(outputFile).required()},
        [&] {
            VirtualClock clock(VirtualClock::REAL_TIME);
            auto cfg = configOption.getConfig();

            // Set up for quick in-memory no-catchup mode.
            cfg.QUORUM_INTERSECTION_CHECKER = false;
            cfg.setInMemoryMode();
            cfg.MODE_DOES_CATCHUP = false;

            auto app = Application::create(clock, cfg, false);
            app->start();
            auto const& lm = app->getLedgerManager();
            auto const& hm = app->getHistoryManager();
            auto& io = clock.getIOContext();
            asio::io_context::work mainWork(io);
            LedgerNumHashPair authPair;
            auto tryCheckpoint = [&](uint32_t seq, Hash h) {
                if (hm.isLastLedgerInCheckpoint(seq))
                {
                    LOG_INFO(
                        DEFAULT_LOG,
                        "Found authenticated checkpoint hash {} for ledger {}",
                        hexAbbrev(h), seq);
                    authPair.first = seq;
                    authPair.second = std::make_optional<Hash>(h);
                }
                else if (authPair.first != seq)
                {
                    authPair.first = seq;
                    LOG_INFO(DEFAULT_LOG,
                             "Ledger {} is not a checkpoint boundary, waiting.",
                             seq);
                }
            };

            if (startLedger != 0 && !startHash.empty())
            {
                Hash h = hexToBin256(startHash);
                tryCheckpoint(startLedger, h);
            }

            while (!(io.stopped() || authPair.second))
            {
                clock.crank();
                if (lm.isSynced())
                {
                    auto const& lhe = lm.getLastClosedLedgerHeader();
                    tryCheckpoint(lhe.header.ledgerSeq, lhe.hash);
                }
            }
            if (authPair.second)
            {
                app->getOverlayManager().shutdown();
                app->getHerder().shutdown();
                app->getWorkScheduler()
                    .executeWork<WriteVerifiedCheckpointHashesWork>(authPair,
                                                                    outputFile);
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
runNewDB(CommandLineArgs const& args)
{
    CommandLine::ConfigOption configOption;
    bool minimalForInMemoryMode = false;

    auto minimalDBParser = [](bool& minimalForInMemoryMode) {
        return clara::Opt{
            minimalForInMemoryMode}["--minimal-for-in-memory-mode"](
            "Reset the special database used only for in-memory mode (see "
            "--in-memory flag");
    };

    return runWithHelp(args,
                       {configurationParser(configOption),
                        minimalDBParser(minimalForInMemoryMode)},
                       [&] {
                           auto cfg = configOption.getConfig();
                           if (minimalForInMemoryMode)
                           {
                               cfg.DATABASE =
                                   SecretValue{minimalDBForInMemoryMode(cfg)};
                           }
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
        showOfflineInfo(configOption.getConfig());
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
    bool inMemory = false;
    bool waitForConsensus = false;
    uint32_t startAtLedger = 0;
    std::string startAtHash;

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
                    cfg.MODE_USES_IN_MEMORY_LEDGER = false;
                    cfg.MODE_ENABLES_BUCKETLIST = false;
                    cfg.PREFETCH_BATCH_SIZE = 0;
                }

                maybeEnableInMemoryMode(cfg, inMemory, startAtLedger,
                                        startAtHash,
                                        /* persistMinimalData */ true);
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
                // Note that when in in-memory mode, additional setup may be
                // required (such as database reset, catchup, etc)
                clock = std::make_shared<VirtualClock>(clockMode);
                app = setupApp(cfg, *clock, startAtLedger, startAtHash);
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
    std::cout << STELLAR_CORE_VERSION << std::endl;
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

int
runGenerateOrSimulateTxs(CommandLineArgs const& args, bool generate)
{
    CommandLine::ConfigOption configOption;
    // If >0, packs mMaxOperations into a ledger maintaining a sustained load
    // If 0, scale the existing ledgers with multiplier
    uint32_t opsPerLedger = 0;
    uint32_t firstLedgerInclusive = 0;
    uint32_t lastLedgerInclusive = 0;
    std::string historyArchiveGet;
    bool upgrade = false;
    // Default to no simulated transactions, just real data
    uint32_t multiplier = 1;
    bool verifyResults = false;

    auto validateFirstLedger = [&] {
        if (firstLedgerInclusive == 0)
        {
            return "first ledger must not be 0";
        }
        else if (firstLedgerInclusive > lastLedgerInclusive)
        {
            return "last ledger must not precede first ledger";
        }
        return "";
    };
    ParserWithValidation firstLedgerParser{
        clara::Opt{firstLedgerInclusive, "LEDGER"}["--first-ledger-inclusive"](
            "first ledger to read from history archive"),
        validateFirstLedger};

    auto opsPerLedgerParser = [](uint32_t& opsPerLedger) {
        return clara::Opt{opsPerLedger, "OPS-PER-LEDGER"}["--ops-per-ledger"](
            "desired number of operations per ledger, will never be less but "
            "could be up to 100 * multiplier more. If 0, real ledger sizes "
            "from the archive stream are used");
    };

    auto multiplierParser = [](uint32_t& multiplier) {
        return clara::Opt{multiplier,
                          "N"}["--multiplier"]("amplification factor, "
                                               "must be consistent with "
                                               "simulated bucketlist");
    };

    std::vector<ParserWithValidation> parsers = {
        configurationParser(configOption), firstLedgerParser,
        clara::Opt{upgrade}["--upgrade"](
            "upgrade to latest known protocol version"),
        clara::Opt{verifyResults}["--verify"](
            "check results after application and log inconsistencies"),
        clara::Opt{lastLedgerInclusive, "LEDGER"}["--last-ledger-inclusive"](
            "last ledger to read from history archive")};

    // Allow passing multiplier during transaction generation, ops-per-ledger
    // during simulation
    parsers.emplace_back((generate ? multiplierParser(multiplier)
                                   : opsPerLedgerParser(opsPerLedger)));

    return runWithHelp(args, parsers, [&] {
        auto config = configOption.getConfig();
        config.setNoListen();

        auto found = config.HISTORY.find("simulate");
        if (!generate)
        {
            // Check if special `simulate` archive is present in the config
            // If so, ensure we're getting historical data from it exclusively
            if (found != config.HISTORY.end())
            {
                auto simArchive = *found;
                config.HISTORY.clear();
                config.HISTORY[simArchive.first] = simArchive.second;
            }

            LOG_INFO(DEFAULT_LOG, "Publishing is disabled in `simulate` mode");
            config.setNoPublish();
        }
        else if (found == config.HISTORY.end())
        {
            throw std::runtime_error("Must provide writable `simulate` archive "
                                     "when generating transactions");
        }

        VirtualClock clock(VirtualClock::REAL_TIME);
        auto app = Application::create(clock, config, false);
        app->start();

        if (generate && app->getHistoryManager().checkpointContainingLedger(
                            lastLedgerInclusive) == lastLedgerInclusive)
        {
            // Bump last ledger to unblock publish
            ++lastLedgerInclusive;
        }

        auto lr =
            LedgerRange::inclusive(firstLedgerInclusive, lastLedgerInclusive);
        CheckpointRange cr(lr, app->getHistoryManager());
        TmpDir dir(app->getTmpDirManager().tmpDir("simulate"));

        auto downloadLedgers = std::make_shared<BatchDownloadWork>(
            *app, cr, HISTORY_FILE_TYPE_LEDGER, dir);
        auto downloadTransactions = std::make_shared<BatchDownloadWork>(
            *app, cr, HISTORY_FILE_TYPE_TRANSACTIONS, dir);
        auto downloadResults = std::make_shared<BatchDownloadWork>(
            *app, cr, HISTORY_FILE_TYPE_RESULTS, dir);
        auto apply = std::make_shared<txsimulation::TxSimApplyTransactionsWork>(
            *app, dir, lr, app->getConfig().NETWORK_PASSPHRASE, opsPerLedger,
            upgrade, multiplier, verifyResults);
        std::vector<std::shared_ptr<BasicWork>> seq{
            downloadLedgers, downloadTransactions, downloadResults, apply};

        app->getWorkScheduler().executeWork<WorkSequence>(
            "download-simulate-seq", seq);

        // Publish all simulated transactions to a simulated archive to avoid
        // re-generating and signing them
        if (generate)
        {
            publish(app);
        }

        return 0;
    });
}

int
runSimulateTxs(CommandLineArgs const& args)
{
    return runGenerateOrSimulateTxs(args, false);
}

int
runGenerateTxs(CommandLineArgs const& args)
{
    return runGenerateOrSimulateTxs(args, true);
}

int
runSimulateBuckets(CommandLineArgs const& args)
{
    CommandLine::ConfigOption configOption;
    uint32_t ledger = 0;
    uint32_t n = 2;
    std::string hasStr = "";

    ParserWithValidation ledgerParser{
        clara::Arg(ledger, "LEDGER").required(),
        [&] { return ledger > 0 ? "" : "Ledger must be non-zero"; }};

    return runWithHelp(
        args,
        {configurationParser(configOption), ledgerParser,
         clara::Opt{n, "N"}["--multiplier"]("amplification factor"),
         clara::Opt{hasStr, "FILENAME"}["--history-archive-state"](
             "skip directly to application if buckets already generated")},
        [&] {
            auto config = configOption.getConfig();
            config.setNoListen();

            std::shared_ptr<HistoryArchiveState> has;
            if (!hasStr.empty())
            {
                LOG_INFO(DEFAULT_LOG, "Loading state from {}", hasStr);
                has = std::make_shared<HistoryArchiveState>();
                has->load(hasStr);
                config.DISABLE_BUCKET_GC =
                    true; /* Do not wipe out simulated buckets */
            }

            VirtualClock clock(VirtualClock::REAL_TIME);
            auto app = Application::create(clock, config, !has);
            app->start();
            TmpDir dir(app->getTmpDirManager().tmpDir("simulate-buckets-tmp"));

            auto checkpoint =
                app->getHistoryManager().checkpointContainingLedger(ledger);

            auto simulateBuckets =
                std::make_shared<txsimulation::TxSimScaleBucketlistWork>(
                    *app, n, checkpoint, dir, has);

            // Once simulated bucketlist is good to go, download ledgers headers
            // to convince LedgerManager that we have successfully restored
            // ledger state
            auto cr = CheckpointRange::inclusive(
                checkpoint, checkpoint,
                app->getHistoryManager().getCheckpointFrequency());
            auto downloadHeaders = std::make_shared<BatchDownloadWork>(
                *app, cr, HISTORY_FILE_TYPE_LEDGER, dir);

            std::vector<std::shared_ptr<BasicWork>> seq{simulateBuckets,
                                                        downloadHeaders};

            auto w = app->getWorkScheduler().executeWork<WorkSequence>(
                "simulate-bucketlist-application", seq);

            if (w->getState() == BasicWork::State::WORK_SUCCESS)
            {
                // Ensure that LedgerManager's view of LCL is consistent with
                // the bucketlist
                FileTransferInfo ft(dir, HISTORY_FILE_TYPE_LEDGER, checkpoint);
                XDRInputFileStream hdrIn;
                hdrIn.open(ft.localPath_nogz());
                LedgerHeaderHistoryEntry curr;
                // Read the last LedgerHeaderHistoryEntry to use as LCL
                try
                {
                    while (hdrIn && hdrIn.readOne(curr))
                        ;
                }
                catch (xdr::xdr_bad_message_size&)
                {
                    LOG_ERROR(DEFAULT_LOG,
                              "Failed to read LedgerHeaderHistoryEntry. "
                              "Version upgrade is likely required");
                    return 1;
                }

                LOG_INFO(DEFAULT_LOG, "Assuming state for ledger {}",
                         curr.header.ledgerSeq);
                app->getLedgerManager().setLastClosedLedger(curr, true);
                app->getBucketManager().forgetUnreferencedBuckets();
            }

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
    FuzzerMode fuzzerMode{FuzzerMode::OVERLAY};
    std::string fuzzerModeArg = "overlay";

    return runWithHelp(args,
                       {logLevelParser(logLevel), metricsParser(metrics),
                        fileNameParser(fileName), outputFileParser(outputFile),
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
#endif

int
handleCommandLine(int argc, char* const* argv)
{
    auto commandLine = CommandLine{
        {{"catchup",
          "execute catchup from history archives without connecting to "
          "network",
          runCatchup},
         {"verify-checkpoints", "write verified checkpoint ledger hashes",
          runWriteVerifiedCheckpointHashes},
         {"convert-id", "displays ID in all known forms", runConvertId},
         {"diag-bucket-stats", "reports statistics on the content of a bucket",
          diagBucketStats},
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
#ifdef BUILD_TESTS
         {"load-xdr", "load an XDR bucket file, for testing", runLoadXDR},
         {"rebuild-ledger-from-buckets",
          "rebuild the current database ledger from the bucket list",
          runRebuildLedgerFromBuckets},
         {"fuzz", "run a single fuzz input and exit", runFuzz},
         {"gen-fuzz", "generate a random fuzzer input file", runGenFuzz},
         {"generate-transactions",
          "generate simulated transactions based on a multiplier. Ensure a "
          "proper writeable test archive is configured",
          runGenerateTxs},
         {"simulate-transactions",
          "simulate applying ledgers from a real or simulated archive (must be "
          "caught up)",
          runSimulateTxs},
         {"simulate-bucketlist", "simulate bucketlist", runSimulateBuckets},
         {"test", "execute test suite", runTest},
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
