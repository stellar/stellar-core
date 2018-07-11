// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/CatchupWorkTests.h"
#include "catchup/CatchupConfiguration.h"
#include "catchup/CatchupWork.h"
#include "ledger/CheckpointRange.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Timer.h"
#include <lib/catch.hpp>
#include <lib/util/format.h>

using namespace stellar;

auto max = std::numeric_limits<uint32_t>::max();
namespace stellar
{
std::vector<std::pair<uint32_t, CatchupConfiguration>> gCatchupRangeCases{
    // fresh database
    // catchup to ledger in middle of first checkpoint
    {1, {2, 0}},
    {1, {2, 1}},
    {1, {2, max}},
    // catchup to ledger at the end of first checkpoint
    {1, {63, 0}},
    {1, {63, 1}},
    {1, {63, 2}},
    {1, {63, max}},
    // catchup to ledger at start of second checkpoint
    {1, {64, 0}},
    {1, {64, 1}},
    {1, {64, 2}},
    {1, {64, 3}},
    {1, {64, max}},
    // catchup to ledger at end of some checkpoint
    {1, {191, 0}},
    {1, {191, 1}},
    {1, {191, 2}},
    {1, {191, 65}},
    {1, {191, 66}},
    {1, {191, 128}},
    {1, {191, max}},
    // catchup to ledger at start of some checkpoint
    {1, {320, 0}},
    {1, {320, 1}},
    {1, {320, 2}},
    {1, {320, 3}},
    {1, {320, 66}},
    {1, {320, 67}},
    {1, {320, 319}},
    {1, {320, 320}},
    {1, {320, max}},

    // almost one checkpoint in database
    // catchup to ledger at the end of first checkpoint
    {62, {63, 0}},
    {62, {63, 1}},
    {62, {63, 2}},
    {62, {63, max}},
    // catchup to ledger at start of second checkpoint
    {62, {64, 0}},
    {62, {64, 1}},
    {62, {64, max}},
    // catchup to ledger at end of some checkpoint
    {62, {319, 0}},
    {62, {319, 1}},
    {62, {319, 65}},
    {62, {319, 66}},
    {62, {319, 129}},
    {62, {319, 130}},
    {62, {319, 319}},
    {62, {319, max}},
    // catchup to ledger at start of some checkpoint
    {62, {320, 0}},
    {62, {320, 1}},
    {62, {320, 66}},
    {62, {320, 67}},
    {62, {320, 319}},
    {62, {320, 320}},
    {62, {320, max}},

    // one checkpoint in database
    // catchup to ledger at start of second checkpoint
    {63, {64, 0}},
    {63, {64, 1}},
    {63, {64, max}},
    // catchup to ledger at end of some checkpoint
    {63, {319, 0}},
    {63, {319, 1}},
    {63, {319, 65}},
    {63, {319, 66}},
    {63, {319, 129}},
    {63, {319, 130}},
    {63, {319, 319}},
    {63, {319, max}},
    // catchup to ledger at start of some checkpoint
    {63, {320, 0}},
    {63, {320, 1}},
    {63, {320, 66}},
    {63, {320, 67}},
    {63, {320, 319}},
    {63, {320, 320}},
    {63, {320, max}},

    // one checkpoint and one ledger in database
    // catchup to ledger at start of second checkpoint
    {64, {64, 0}},
    {64, {64, 1}},
    {64, {64, max}},
    // catchup to ledger at end of some checkpoint
    {64, {319, 0}},
    {64, {319, 1}},
    {64, {319, 65}},
    {64, {319, 66}},
    {64, {319, 319}},
    {64, {319, max}},
    // catchup to ledger at start of some checkpoint
    {64, {320, 0}},
    {64, {320, 1}},
    {64, {320, 66}},
    {64, {320, 67}},
    {64, {320, 319}},
    {64, {320, 320}},
    {64, {320, max}}};
}

TEST_CASE("compute CatchupRange from CatchupConfiguration", "[catchupWork]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    auto& historyManager = app->getHistoryManager();

    for (auto const& test : gCatchupRangeCases)
    {
        auto lastClosedLedger = test.first;
        auto configuration = test.second;

        auto sectionName = fmt::format(
            "lcl = {}, to ledger = {}, count = {}", lastClosedLedger,
            configuration.toLedger(), configuration.count());
        SECTION(sectionName)
        {
            auto range = CatchupWork::makeCatchupRange(
                lastClosedLedger, configuration, historyManager);

            // we need to finish where we wanted to finish
            REQUIRE(configuration.toLedger() == range.first.last());

            // if LCL was later than our first checkpoint, we would either
            // apply buckets at or before LCL, or do a lot of unnecessary
            // attempts to apply transactions
            REQUIRE(lastClosedLedger <= range.first.first());

            if (range.second)
            {
                // this contains 1 bucket apply operation and
                // stopAt - firstCheckpoint ledger apply operations
                auto appliedCount =
                    range.first.last() - range.first.first() + 1;

                // we aren't applying buckets just after lcl...
                REQUIRE(range.first.first() > lastClosedLedger + 1);

                // we are applying at least configuration.count()
                REQUIRE(appliedCount >= configuration.count());

                if (std::numeric_limits<uint32_t>::max() -
                        historyManager.getCheckpointFrequency() >=
                    configuration.count())
                {
                    // but at most count + getCheckpointFrequency
                    // doing more would mean we are doing non-needed work
                    REQUIRE(appliedCount <=
                            configuration.count() +
                                historyManager.getCheckpointFrequency());
                }
            }
            else
            {
                // if buckets are not applied we need to apply ledgers
                // starting from lastClosedLedger + 1, so lastClosedLedger + 1
                // must be in first checkpoint
                auto checkpointRange =
                    CheckpointRange{range.first, historyManager};
                REQUIRE(lastClosedLedger +
                            historyManager.getCheckpointFrequency() >=
                        checkpointRange.first());
                if (std::numeric_limits<uint32_t>::max() -
                        historyManager.getCheckpointFrequency() >=
                    configuration.count())
                {
                    // and we are applying at most count() +
                    //   getCheckpointFrequency() ledgers
                    // more would mean we are doing non-needed work
                    // less just means that lastClosedLedger is too close to
                    // target ledger
                    auto appliedCount = range.first.last() - lastClosedLedger;
                    REQUIRE(appliedCount <=
                            configuration.count() +
                                historyManager.getCheckpointFrequency());
                }
            }
        }
    }
}
