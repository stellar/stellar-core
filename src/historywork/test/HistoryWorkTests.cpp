// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "history/HistoryArchiveManager.h"
#include "history/test/HistoryTestsUtils.h"
#include "historywork/WriteVerifiedCheckpointHashesWork.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "work/WorkScheduler.h"
#include <lib/catch.hpp>
#include <lib/json/json.h>

using namespace stellar;
using namespace historytestutils;

TEST_CASE("write verified checkpoint hashes", "[historywork]")
{
    CatchupSimulation catchupSimulation{};
    uint32_t nestedBatchSize = 4;
    auto checkpointLedger =
        catchupSimulation.getLastCheckpointLedger(5 * nestedBatchSize);
    catchupSimulation.ensureOnlineCatchupPossible(checkpointLedger,
                                                  5 * nestedBatchSize);

    std::vector<LedgerNumHashPair> pairs =
        catchupSimulation.getAllPublishedCheckpoints();
    LedgerNumHashPair pair = pairs.back();
    auto tmpDir = catchupSimulation.getApp().getTmpDirManager().tmpDir(
        "write-checkpoint-hashes-test");
    auto file = tmpDir.getName() + "/verified-ledgers.json";
    auto& wm = catchupSimulation.getApp().getWorkScheduler();
    {
        auto w = wm.executeWork<WriteVerifiedCheckpointHashesWork>(
            pair, file, nestedBatchSize);
        REQUIRE(w->getState() == BasicWork::State::WORK_SUCCESS);
    }
    // Make sure w is destroyed.
    wm.shutdown();
    while (wm.getState() != BasicWork::State::WORK_ABORTED)
    {
        catchupSimulation.getClock().crank();
    }

    for (auto const& p : pairs)
    {
        LOG_DEBUG(DEFAULT_LOG, "Verified {} with hash {}", p.first,
                  hexAbbrev(*p.second));
        Hash h = WriteVerifiedCheckpointHashesWork::loadHashFromJsonOutput(
            p.first, file);
        REQUIRE(h == *p.second);
    }
}
