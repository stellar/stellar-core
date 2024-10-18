// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "history/HistoryArchiveManager.h"
#include "history/test/HistoryTestsUtils.h"
#include "historywork/CheckSingleLedgerHeaderWork.h"
#include "historywork/WriteVerifiedCheckpointHashesWork.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "work/WorkScheduler.h"
#include <lib/catch.hpp>
#include <lib/json/json.h>

#include <iostream>

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
    std::string file = tmpDir.getName() + "/verified-ledgers.json";
    auto& wm = catchupSimulation.getApp().getWorkScheduler();
    std::optional<std::uint32_t> noFromLedger = std::nullopt;
    std::optional<std::string> noVerifiedLedgerFile = std::nullopt;
    std::optional<LedgerNumHashPair> noLatestTrustedHashPair = std::nullopt;

    size_t startingPairIdx = 0;
    {
        SECTION("from genesis")
        {
            auto w = wm.executeWork<WriteVerifiedCheckpointHashesWork>(
                pair, file, noVerifiedLedgerFile, noLatestTrustedHashPair,
                noFromLedger, nestedBatchSize);
            REQUIRE(w->getState() == BasicWork::State::WORK_SUCCESS);
        }
        SECTION("from specified ledger")
        {
            startingPairIdx = 1;
            std::optional<std::uint32_t> fromLedger =
                pairs[startingPairIdx].first;
            auto w = wm.executeWork<WriteVerifiedCheckpointHashesWork>(
                pair, file, noVerifiedLedgerFile, noLatestTrustedHashPair,
                fromLedger, nestedBatchSize);
            REQUIRE(w->getState() == BasicWork::State::WORK_SUCCESS);
        }
    }

    auto checkFileContents = [](const auto& pairs, auto startingPairIdx,
                                std::string file) {
        for (size_t i = 0; i < pairs.size(); ++i)
        {
            auto p = pairs[i];
            LOG_DEBUG(DEFAULT_LOG, "Verified {} with hash {}", p.first,
                      hexAbbrev(*p.second));
            Hash h = WriteVerifiedCheckpointHashesWork::loadHashFromJsonOutput(
                p.first, file);
            // If we did not start from the beginning, the hashes before the
            // starting pair should not be in the file.
            if (i < startingPairIdx)
            {
                REQUIRE(h == Hash{});
            }
            else
            {
                REQUIRE(h == *p.second);
            }
        }
        // Check that the "latest" ledger in the file is the same as the last
        // pair in the pairs vector.
        auto latest =
            WriteVerifiedCheckpointHashesWork::loadLatestHashPairFromJsonOutput(
                file);
        REQUIRE(latest.first == pairs.back().first);
    };

    checkFileContents(pairs, startingPairIdx, file);

    // Advance the simulation.
    auto secondCheckpointLedger =
        catchupSimulation.getLastCheckpointLedger(10 * nestedBatchSize);
    catchupSimulation.ensureOnlineCatchupPossible(secondCheckpointLedger,
                                                  5 * nestedBatchSize);
    pairs = catchupSimulation.getAllPublishedCheckpoints();

    std::optional<std::string> trustedHashFile = file;
    std::optional<LedgerNumHashPair> latestTrustedHashPair =
        WriteVerifiedCheckpointHashesWork::loadLatestHashPairFromJsonOutput(
            file);
    file += ".new";
    // Run work again with existing file.
    {
        auto w = wm.executeWork<WriteVerifiedCheckpointHashesWork>(
            pairs.back(), file, trustedHashFile, latestTrustedHashPair,
            noFromLedger, nestedBatchSize);
        REQUIRE(w->getState() == BasicWork::State::WORK_SUCCESS);
    }

    // Ensure the file contains all pairs, from the first run and the second.
    checkFileContents(pairs, startingPairIdx, file);
}

TEST_CASE("check single ledger header work", "[historywork]")
{
    CatchupSimulation catchupSimulation{};
    auto l1 = catchupSimulation.getLastCheckpointLedger(2);
    auto l2 = catchupSimulation.getLastCheckpointLedger(4);
    catchupSimulation.ensureOfflineCatchupPossible(l1);
    auto& app = catchupSimulation.getApp();
    auto& lm = app.getLedgerManager();
    auto lhhe = lm.getLastClosedLedgerHeader();
    catchupSimulation.ensureOfflineCatchupPossible(l2);
    auto arch =
        app.getHistoryArchiveManager().selectRandomReadableHistoryArchive();
    auto& wm = app.getWorkScheduler();
    auto w = wm.executeWork<CheckSingleLedgerHeaderWork>(arch, lhhe);
    REQUIRE(w->getState() == BasicWork::State::WORK_SUCCESS);
}