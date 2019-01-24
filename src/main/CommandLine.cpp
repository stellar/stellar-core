// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/CommandLine.h"
#include "catchup/CatchupConfiguration.h"
#include "history/InferredQuorumUtils.h"
#include "main/Application.h"
#include "main/ApplicationUtils.h"
#include "main/Config.h"
#include "main/StellarCoreVersion.h"
#include "main/dumpxdr.h"
#include "main/fuzz.h"
#include "scp/QuorumSetUtils.h"
#include "test/test.h"
#include "util/Logging.h"
#include "util/optional.h"

#include <iostream>
#include <lib/clara.hpp>
#include <lib/util/format.h>

namespace stellar
{

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

        el::Level mLogLevel{el::Level::Info};
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
    optional<Command> selectCommand(std::string const& commandName);
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
    ParserWithValidation(clara::Parser parser,
                         std::function<std::string()> isValid = [] {
                             return std::string{};
                         })
    {
        mParser = parser;
        mIsValid = isValid;
    }

    ParserWithValidation(clara::Arg arg, std::function<std::string()> isValid =
                                             [] { return std::string{}; })
    {
        mParser = clara::Parser{} | arg;
        mIsValid = isValid;
    }

    ParserWithValidation(clara::Opt opt, std::function<std::string()> isValid =
                                             [] { return std::string{}; })
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
outputFileParser(std::string& string)
{
    return clara::Opt{string, "FILE-NAME"}["--output-file"]("output file");
}

clara::Opt
logLevelParser(el::Level& value)
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
base64Parser(bool& base64)
{
    return clara::Opt{base64}["--base64"]("use base64");
}

clara::Opt
disableBucketGCParser(bool& disableBucketGC)
{
    return clara::Opt{disableBucketGC}["--disable-bucket-gc"](
        "keeps all, even old, buckets on disk");
}

clara::Parser
configurationParser(CommandLine::ConfigOption& configOption)
{
    return logLevelParser(configOption.mLogLevel) |
           metricsParser(configOption.mMetrics) |
           clara::Opt{configOption.mConfigFile, "FILE-NAME"}["--conf"](
               "specify a config file ('-' for STDIN, "
               "default 'stellar-core.cfg')");
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
parseCatchup(std::string const& catchup)
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
        return {parseLedger(catchup.substr(0, separatorIndex)),
                parseLedgerCount(catchup.substr(separatorIndex + 1))};
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

    LOG(INFO) << "Config from " << configFile;

    // yes you really have to do this 3 times
    Logging::setLogLevel(mLogLevel, nullptr);
    config.load(configFile);

    Logging::setFmt(KeyUtils::toShortString(config.NODE_SEED.getPublicKey()));
    Logging::setLogLevel(mLogLevel, nullptr);

    if (logToFile)
    {
        if (config.LOG_FILE_PATH.size())
            Logging::setLoggingToFile(config.LOG_FILE_PATH);
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

optional<CommandLine::Command>
CommandLine::selectCommand(std::string const& commandName)
{
    if (commandName.empty())
    {
        return nullopt<Command>();
    }

    auto command = std::find_if(
        std::begin(mCommands), std::end(mCommands),
        [&](Command const& command) { return command.name() == commandName; });
    if (command != std::end(mCommands))
    {
        return make_optional<Command>(*command);
    }

    command = std::find_if(
        std::begin(mCommands), std::end(mCommands),
        [&](Command const& command) { return command.name() == "help"; });
    if (command != std::end(mCommands))
    {
        return make_optional<Command>(*command);
    }
    return nullopt<Command>();
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
runCatchup(CommandLineArgs const& args)
{
    CommandLine::ConfigOption configOption;
    std::string catchupString;
    std::string outputFile;

    auto validateCatchupString = [&] {
        try
        {
            parseCatchup(catchupString);
            return std::string{};
        }
        catch (std::runtime_error& e)
        {
            return std::string{e.what()};
        }
    };
    auto catchupStringParser = ParserWithValidation{
        clara::Arg(catchupString, "DESTINATION-LEDGER/LEDGER-COUNT").required(),
        validateCatchupString};
    auto disableBucketGC = false;

    return runWithHelp(
        args,
        {configurationParser(configOption), catchupStringParser,
         outputFileParser(outputFile), disableBucketGCParser(disableBucketGC)},
        [&] {
            auto config = configOption.getConfig();
            config.setNoListen();
            config.DISABLE_BUCKET_GC = disableBucketGC;

            VirtualClock clock(VirtualClock::REAL_TIME);
            auto app = Application::create(clock, config, false);
            Json::Value catchupInfo;
            auto result =
                catchup(app, parseCatchup(catchupString), catchupInfo);
            if (!catchupInfo.isNull())
                writeCatchupInfo(catchupInfo, outputFile);

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
runCheckQuorum(CommandLineArgs const& args)
{
    CommandLine::ConfigOption configOption;

    return runWithHelp(args, {configurationParser(configOption)}, [&] {
        checkQuorumIntersection(configOption.getConfig());
        return 0;
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

    return runWithHelp(args, {fileNameParser(xdr)}, [&] {
        dumpXdrStream(xdr);
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
                           setForceSCPFlag(configOption.getConfig(), !reset);
                           return 0;
                       });
}

int
runFuzz(CommandLineArgs const& args)
{
    el::Level logLevel{el::Level::Info};
    std::vector<std::string> metrics;
    std::string fileName;

    return runWithHelp(args,
                       {logLevelParser(logLevel), metricsParser(metrics),
                        fileNameParser(fileName)},
                       [&] {
                           fuzz(fileName, logLevel, metrics);
                           return 0;
                       });
}

int
runGenFuzz(CommandLineArgs const& args)
{
    std::string fileName;

    return runWithHelp(args, {fileNameParser(fileName)}, [&] {
        genfuzz(fileName);
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
runInferQuorum(CommandLineArgs const& args)
{
    CommandLine::ConfigOption configOption;

    return runWithHelp(args, {configurationParser(configOption)}, [&] {
        inferQuorumAndWrite(configOption.getConfig());
        return 0;
    });
}

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
runNewDB(CommandLineArgs const& args)
{
    CommandLine::ConfigOption configOption;

    return runWithHelp(args, {configurationParser(configOption)}, [&] {
        initializeDatabase(configOption.getConfig());
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

    auto fileTypeOpt = clara::Opt(fileType, "FILE-TYPE")["--filetype"](
        "[auto|ledgerheader|meta|result|resultpair|tx|txfee]");

    return runWithHelp(
        args, {fileNameParser(xdr), fileTypeOpt, base64Parser(base64)}, [&] {
            printXdr(xdr, fileType, base64);
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

    return runWithHelp(args,
                       {configurationParser(configOption),
                        disableBucketGCParser(disableBucketGC)},
                       [&] {
                           Config cfg;
                           try
                           {
                               cfg = configOption.getConfig();
                               cfg.DISABLE_BUCKET_GC = disableBucketGC;
                           }
                           catch (std::exception& e)
                           {
                               LOG(FATAL) << "Got an exception: " << e.what();
                               return 1;
                           }
                           // run outside of catch block so that we properly
                           // capture crashes
                           return runWithConfig(cfg);
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

int
runWriteQuorum(CommandLineArgs const& args)
{
    CommandLine::ConfigOption configOption;
    std::string outputFile;

    return runWithHelp(
        args, {configurationParser(configOption), outputFileParser(outputFile)},
        [&] {
            writeQuorumGraph(configOption.getConfig(), outputFile);
            return 0;
        });
}

optional<int>
handleCommandLine(int argc, char* const* argv)
{
    auto commandLine = CommandLine{
        {{"catchup",
          "execute catchup from history archives without connecting to "
          "network",
          runCatchup},
         {"check-quorum", "check quorum intersection from history",
          runCheckQuorum},
         {"convert-id", "displays ID in all known forms", runConvertId},
         {"dump-xdr", "dump an XDR file, for debugging", runDumpXDR},
         {"force-scp",
          "next time stellar-core is run, SCP will start with "
          "the local ledger rather than waiting to hear from the "
          "network",
          runForceSCP},
         {"fuzz", "run a single fuzz input and exit", runFuzz},
         {"gen-fuzz", "generate a random fuzzer input file", runGenFuzz},
         {"gen-seed", "generate and print a random node seed", runGenSeed},
         {"http-command", "send a command to local stellar-core",
          runHttpCommand},
         {"infer-quorum", "print a quorum set inferred from history",
          runInferQuorum},
         {"load-xdr", "load an XDR bucket file, for testing", runLoadXDR},
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
         {"test", "execute test suite", runTest},
         {"write-quorum", "print a quorum set graph from history",
          runWriteQuorum},
         {"version", "print version information", runVersion}}};

    auto ajustedCommandLine = commandLine.adjustCommandLine({argc, argv});
    auto command = commandLine.selectCommand(ajustedCommandLine.first);
    if (!command)
    {
        return nullopt<int>();
    }

    auto exeName = "stellar-core";
    auto commandName = fmt::format("{0} {1}", exeName, command->name());
    auto args = CommandLineArgs{exeName, commandName, command->description(),
                                ajustedCommandLine.second};
    if (command->name() == "run")
    {
        // run outside of catch block so that we properly capture crashes
        return make_optional<int>(command->run(args));
    }

    try
    {
        return make_optional<int>(command->run(args));
    }
    catch (std::exception& e)
    {
        LOG(FATAL) << "Got an exception: " << e.what();
        return make_optional<int>(1);
    }
}
}
