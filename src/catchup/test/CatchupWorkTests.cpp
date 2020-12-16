// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/test/CatchupWorkTests.h"
#include "catchup/CatchupConfiguration.h"
#include "catchup/CatchupRange.h"
#include "catchup/CatchupWork.h"
#include "ledger/CheckpointRange.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include <fmt/format.h>
#include <lib/catch.hpp>

using namespace stellar;

auto max = std::numeric_limits<uint32_t>::max();
namespace stellar
{
std::vector<std::pair<uint32_t, CatchupConfiguration>> gCatchupRangeCases{
    // fresh database
    // catchup to ledger in middle of first checkpoint
    {1, {2, 0, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {1, {2, 1, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {1, {2, max, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    // catchup to ledger at the end of first checkpoint
    {1, {63, 0, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {1, {63, 1, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {1, {63, 2, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {1, {63, max, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    // catchup to ledger at start of second checkpoint
    {1, {64, 0, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {1, {64, 1, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {1, {64, 2, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {1, {64, 3, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {1, {64, max, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    // catchup to ledger at end of some checkpoint
    {1, {191, 0, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {1, {191, 1, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {1, {191, 2, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {1, {191, 65, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {1, {191, 66, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {1, {191, 128, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {1, {191, max, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    // catchup to ledger at start of some checkpoint
    {1, {320, 0, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {1, {320, 1, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {1, {320, 2, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {1, {320, 3, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {1, {320, 66, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {1, {320, 67, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {1, {320, 319, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {1, {320, 320, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {1, {320, max, CatchupConfiguration::Mode::OFFLINE_BASIC}},

    // almost one checkpoint in database
    // catchup to ledger at the end of first checkpoint
    {62, {63, 0, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {62, {63, 1, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {62, {63, 2, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {62, {63, max, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    // catchup to ledger at start of second checkpoint
    {62, {64, 0, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {62, {64, 1, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {62, {64, max, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    // catchup to ledger at end of some checkpoint
    {62, {319, 0, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {62, {319, 1, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {62, {319, 65, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {62, {319, 66, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {62, {319, 129, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {62, {319, 130, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {62, {319, 319, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {62, {319, max, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    // catchup to ledger at start of some checkpoint
    {62, {320, 0, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {62, {320, 1, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {62, {320, 66, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {62, {320, 67, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {62, {320, 319, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {62, {320, 320, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {62, {320, max, CatchupConfiguration::Mode::OFFLINE_BASIC}},

    // one checkpoint in database
    // catchup to ledger at start of second checkpoint
    {63, {64, 0, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {63, {64, 1, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {63, {64, max, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    // catchup to ledger at end of some checkpoint
    {63, {319, 0, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {63, {319, 1, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {63, {319, 65, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {63, {319, 66, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {63, {319, 129, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {63, {319, 130, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {63, {319, 319, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {63, {319, max, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    // catchup to ledger at start of some checkpoint
    {63, {320, 0, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {63, {320, 1, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {63, {320, 66, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {63, {320, 67, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {63, {320, 319, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {63, {320, 320, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {63, {320, max, CatchupConfiguration::Mode::OFFLINE_BASIC}},

    // one checkpoint and one ledger in database
    // catchup to ledger at end of some checkpoint
    {64, {319, 0, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {64, {319, 1, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {64, {319, 65, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {64, {319, 66, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {64, {319, 319, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {64, {319, max, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    // catchup to ledger at start of some checkpoint
    {64, {320, 0, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {64, {320, 1, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {64, {320, 66, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {64, {320, 67, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {64, {320, 319, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {64, {320, 320, CatchupConfiguration::Mode::OFFLINE_BASIC}},
    {64, {320, max, CatchupConfiguration::Mode::OFFLINE_BASIC}}};
}

TEST_CASE("compute CatchupRange from CatchupConfiguration", "[catchup]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    auto& historyManager = app->getHistoryManager();

    for (auto const& test : gCatchupRangeCases)
    {
        auto lastClosedLedger = test.first;
        auto configuration = test.second;

        auto name = fmt::format("lcl = {}, to ledger = {}, count = {}",
                                lastClosedLedger, configuration.toLedger(),
                                configuration.count());
        CLOG_DEBUG(History, "Catchup configuration: {}", name);
        {
            auto range =
                CatchupRange{lastClosedLedger, configuration, historyManager};

            CLOG_DEBUG(History,
                       "computed CatchupRange: first={}, last={}, "
                       "replayFirst={}, replayCount={}, replayLimit={}",
                       range.first(), range.last(), range.getReplayFirst(),
                       range.getReplayCount(), range.getReplayLimit());

            // we need to finish where we wanted to finish
            REQUIRE(configuration.toLedger() == range.last());

            if (range.applyBuckets())
            {
                // we only apply buckets when lcl is GENESIS
                REQUIRE(lastClosedLedger == LedgerManager::GENESIS_LEDGER_SEQ);

                // buckets can only by applied on checkpoint boundary
                REQUIRE(historyManager.isLastLedgerInCheckpoint(
                    range.getBucketApplyLedger()));

                // If we're applying buckets and replaying ledgers, we do
                // the latter immediately after the former.
                if (range.applyBuckets() && range.replayLedgers())
                {
                    REQUIRE(range.getReplayFirst() ==
                            range.getBucketApplyLedger() + 1);
                }

                // Check that we're covering the first ledger implied by the
                // user-provided last-ledger/count.
                if (configuration.count() <= configuration.toLedger())
                {
                    uint32_t intendedFirst =
                        configuration.toLedger() - configuration.count();
                    LedgerRange fullRange =
                        range.getFullRangeIncludingBucketApply();
                    REQUIRE(fullRange.mFirst <= intendedFirst);
                    REQUIRE(intendedFirst < fullRange.limit());
                }

                // we are applying at least configuration.count() ledgers
                REQUIRE(range.count() >= configuration.count());

                if (std::numeric_limits<uint32_t>::max() -
                        historyManager.getCheckpointFrequency() >=
                    configuration.count())
                {
                    // but at most count + getCheckpointFrequency
                    // doing more would mean we are doing non-needed work
                    REQUIRE(range.getReplayCount() <=
                            configuration.count() +
                                historyManager.getCheckpointFrequency());
                }
            }
            else
            {
                // we apply ledgers just after LCL
                REQUIRE(lastClosedLedger + 1 == range.getReplayFirst());
            }
        }
    }
}

TEST_CASE("CatchupRange starting on checkpoint boundary still replays it",
          "[catchup]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    auto& historyManager = app->getHistoryManager();

    uint32_t lcl = 1;

    // 66/4 means user wants replay of 63,64,65,66 which means
    // we must start from a state from _before_ 63, namely LCL.
    CatchupConfiguration conf1{66, 4,
                               CatchupConfiguration::Mode::OFFLINE_BASIC};
    CatchupRange crange1{lcl, conf1, historyManager};

    REQUIRE(!crange1.applyBuckets());
    REQUIRE(crange1.replayLedgers());
    REQUIRE(crange1.getReplayFirst() == 2);
    REQUIRE(crange1.getReplayCount() == 65);

    // 66/3 means user wants replay of 64,65,66 which means
    // we must start from a state _before_ 64, namely 63.
    CatchupConfiguration conf2{66, 3,
                               CatchupConfiguration::Mode::OFFLINE_BASIC};
    CatchupRange crange2{lcl, conf2, historyManager};

    REQUIRE(crange2.applyBuckets());
    REQUIRE(crange2.replayLedgers());
    REQUIRE(crange2.getBucketApplyLedger() == 63);
    REQUIRE(crange2.getReplayFirst() == 64);
    REQUIRE(crange2.getReplayCount() == 3);
}