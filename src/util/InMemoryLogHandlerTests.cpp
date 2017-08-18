// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/InMemoryLogHandler.h"
#include <lib/catch.hpp>

namespace stellar
{

// Trace < Debug < Info < Warning < Error < Fatal < None

TEST_CASE("returns true when given expected level", "[in_memory_log_handler]")
{
    using el::Level;
    LogFlushPredicate predicate;
    auto startLevel = el::LevelHelper::castToInt(el::Level::Trace);

    el::LevelHelper::forEachLevel(
        &startLevel, [&startLevel, &predicate]() -> bool {
            Level thisLevel = el::LevelHelper::castFromInt(startLevel);
            if (Level::Global == thisLevel || Level::Verbose == thisLevel ||
                Level::Unknown == thisLevel)
            {
                return false;
            }

            REQUIRE(predicate.compareLessOrEqual(thisLevel, thisLevel) == true);

            return false;
        });
}

TEST_CASE("returns false when given lower level", "[in_memory_log_handler]")
{
    using el::Level;
    using el::LogMessage;
    using el::LogDispatchData;
    using el::base::DispatchAction;

    LogFlushPredicate predicate;

    REQUIRE(predicate.compareLessOrEqual(Level::Fatal, Level::Error) == false);
    REQUIRE(predicate.compareLessOrEqual(Level::Fatal, Level::Warning) ==
            false);
    REQUIRE(predicate.compareLessOrEqual(Level::Fatal, Level::Info) == false);
    REQUIRE(predicate.compareLessOrEqual(Level::Fatal, Level::Debug) == false);
    REQUIRE(predicate.compareLessOrEqual(Level::Fatal, Level::Trace) == false);

    REQUIRE(predicate.compareLessOrEqual(Level::Error, Level::Warning) ==
            false);
    REQUIRE(predicate.compareLessOrEqual(Level::Error, Level::Info) == false);
    REQUIRE(predicate.compareLessOrEqual(Level::Error, Level::Debug) == false);
    REQUIRE(predicate.compareLessOrEqual(Level::Error, Level::Trace) == false);

    REQUIRE(predicate.compareLessOrEqual(Level::Warning, Level::Info) == false);
    REQUIRE(predicate.compareLessOrEqual(Level::Warning, Level::Debug) ==
            false);
    REQUIRE(predicate.compareLessOrEqual(Level::Warning, Level::Trace) ==
            false);

    REQUIRE(predicate.compareLessOrEqual(Level::Info, Level::Debug) == false);
    REQUIRE(predicate.compareLessOrEqual(Level::Info, Level::Trace) == false);

    REQUIRE(predicate.compareLessOrEqual(Level::Debug, Level::Trace) == false);
}

TEST_CASE("returns true when given higher level", "[in_memory_log_handler]")
{
    using el::Level;
    using el::LogMessage;
    using el::LogDispatchData;
    using el::base::DispatchAction;

    LogFlushPredicate predicate;

    REQUIRE(predicate.compareLessOrEqual(Level::Trace, Level::Fatal) == true);
    REQUIRE(predicate.compareLessOrEqual(Level::Trace, Level::Error) == true);
    REQUIRE(predicate.compareLessOrEqual(Level::Trace, Level::Warning) == true);
    REQUIRE(predicate.compareLessOrEqual(Level::Trace, Level::Info) == true);
    REQUIRE(predicate.compareLessOrEqual(Level::Trace, Level::Debug) == true);

    REQUIRE(predicate.compareLessOrEqual(Level::Debug, Level::Fatal) == true);
    REQUIRE(predicate.compareLessOrEqual(Level::Debug, Level::Error) == true);
    REQUIRE(predicate.compareLessOrEqual(Level::Debug, Level::Warning) == true);
    REQUIRE(predicate.compareLessOrEqual(Level::Debug, Level::Info) == true);

    REQUIRE(predicate.compareLessOrEqual(Level::Info, Level::Fatal) == true);
    REQUIRE(predicate.compareLessOrEqual(Level::Info, Level::Error) == true);
    REQUIRE(predicate.compareLessOrEqual(Level::Info, Level::Warning) == true);

    REQUIRE(predicate.compareLessOrEqual(Level::Warning, Level::Fatal) == true);
    REQUIRE(predicate.compareLessOrEqual(Level::Warning, Level::Error) == true);

    REQUIRE(predicate.compareLessOrEqual(Level::Error, Level::Fatal) == true);
}
}
