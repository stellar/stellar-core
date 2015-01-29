// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "main/Application.h"
#include "util/Logging.h"

/*
Levels:
    TRACE
    DEBUG
    FATAL
    ERROR
    WARNING
    INFO
*/

namespace stellar
{
el::Configurations Logging::mDefaultConf;

void
Logging::setUpLogging(std::string const& filename)
{
    // el::Loggers::addFlag(el::LoggingFlag::HierarchicalLogging);
    el::Loggers::addFlag(el::LoggingFlag::DisableApplicationAbortOnFatalLog);

    el::Loggers::getLogger("FBA");
    el::Loggers::getLogger("CLF");
    el::Loggers::getLogger("Ledger");
    el::Loggers::getLogger("Overlay");
    el::Loggers::getLogger("Herder");
    el::Loggers::getLogger("Tx");

    mDefaultConf.setToDefault();

    mDefaultConf.setGlobally(
        el::ConfigurationType::Format,
        "%datetime{%d/%M/%y %H:%m:%s} [%logger] %level %msg");
    mDefaultConf.setGlobally(el::ConfigurationType::Filename, filename);
    mDefaultConf.set(
        el::Level::Error, el::ConfigurationType::Format,
        "%datetime{%d/%M/%y %H:%m:%s} [%logger] %level %msg [%fbase:%line]");
    mDefaultConf.set(
        el::Level::Trace, el::ConfigurationType::Format,
        "%datetime{%d/%M/%y %H:%m:%s} [%logger] %level %msg [%fbase:%line]");
    mDefaultConf.set(
        el::Level::Fatal, el::ConfigurationType::Format,
        "%datetime{%d/%M/%y %H:%m:%s} [%logger] %level %msg [%fbase:%line]");

    el::Loggers::reconfigureAllLoggers(mDefaultConf);
}

void
Logging::setLogLevel(el::Level level, const char* partition)
{
    el::Configurations config = mDefaultConf;

    if (el::Level::Trace < level)
        config.set(el::Level::Trace, el::ConfigurationType::Enabled, "false");
    if (el::Level::Debug < level)
        config.set(el::Level::Debug, el::ConfigurationType::Enabled, "false");
    if (el::Level::Fatal < level)
        config.set(el::Level::Fatal, el::ConfigurationType::Enabled, "false");
    if (el::Level::Error < level)
        config.set(el::Level::Error, el::ConfigurationType::Enabled, "false");
    if (el::Level::Warning < level)
        config.set(el::Level::Warning, el::ConfigurationType::Enabled, "false");
    if (el::Level::Info < level)
        config.set(el::Level::Info, el::ConfigurationType::Enabled, "false");

    if (partition)
        el::Loggers::reconfigureLogger(partition, config);
    else
        el::Loggers::reconfigureAllLoggers(config);
}
}
