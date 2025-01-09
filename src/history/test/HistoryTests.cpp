// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketManager.h"
#include "bucket/test/BucketTestUtils.h"
#include "catchup/LedgerApplyManagerImpl.h"
#include "catchup/test/CatchupWorkTests.h"
#include "history/CheckpointBuilder.h"
#include "history/FileTransferInfo.h"
#include "history/HistoryArchiveManager.h"
#include "history/HistoryManagerImpl.h"
#include "history/test/HistoryTestsUtils.h"
#include "historywork/GetHistoryArchiveStateWork.h"
#include "historywork/GunzipFileWork.h"
#include "historywork/GzipFileWork.h"
#include "historywork/PutHistoryArchiveStateWork.h"
#include "ledger/LedgerManager.h"
#include "main/Maintainer.h"
#include "main/PersistentState.h"
#include "process/ProcessManager.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "util/Fs.h"
#include "util/Logging.h"
#include "work/WorkScheduler.h"

#include "historywork/BatchDownloadWork.h"
#include "historywork/DownloadBucketsWork.h"
#include "historywork/DownloadVerifyTxResultsWork.h"
#include "historywork/VerifyTxResultsWork.h"
#include <fmt/format.h>
#include <lib/catch.hpp>

using namespace stellar;
using namespace historytestutils;

TEST_CASE("checkpoint containing ledger", "[history]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    // Technically ledger 0 doesn't exist so it's not "in" any checkpoint; but
    // the first checkpoint's ledger range covers ledger 0 so we consider it
    // "contained" in that checkpoint for the sake of this function.
    CHECK(HistoryManager::checkpointContainingLedger(0, app->getConfig()) ==
          0x3f);
    CHECK(HistoryManager::checkpointContainingLedger(1, app->getConfig()) ==
          0x3f);
    CHECK(HistoryManager::checkpointContainingLedger(2, app->getConfig()) ==
          0x3f);
    CHECK(HistoryManager::checkpointContainingLedger(3, app->getConfig()) ==
          0x3f);
    // ...
    CHECK(HistoryManager::checkpointContainingLedger(61, app->getConfig()) ==
          0x3f);
    CHECK(HistoryManager::checkpointContainingLedger(62, app->getConfig()) ==
          0x3f);
    CHECK(HistoryManager::checkpointContainingLedger(63, app->getConfig()) ==
          0x3f);
    CHECK(HistoryManager::checkpointContainingLedger(64, app->getConfig()) ==
          0x7f);
    CHECK(HistoryManager::checkpointContainingLedger(65, app->getConfig()) ==
          0x7f);
    CHECK(HistoryManager::checkpointContainingLedger(66, app->getConfig()) ==
          0x7f);
    // ...
    CHECK(HistoryManager::checkpointContainingLedger(125, app->getConfig()) ==
          0x7f);
    CHECK(HistoryManager::checkpointContainingLedger(126, app->getConfig()) ==
          0x7f);
    CHECK(HistoryManager::checkpointContainingLedger(127, app->getConfig()) ==
          0x7f);
    CHECK(HistoryManager::checkpointContainingLedger(128, app->getConfig()) ==
          0xbf);
    CHECK(HistoryManager::checkpointContainingLedger(129, app->getConfig()) ==
          0xbf);
    CHECK(HistoryManager::checkpointContainingLedger(130, app->getConfig()) ==
          0xbf);
    // ...
    CHECK(HistoryManager::checkpointContainingLedger(189, app->getConfig()) ==
          0xbf);
    CHECK(HistoryManager::checkpointContainingLedger(190, app->getConfig()) ==
          0xbf);
    CHECK(HistoryManager::checkpointContainingLedger(191, app->getConfig()) ==
          0xbf);
    CHECK(HistoryManager::checkpointContainingLedger(192, app->getConfig()) ==
          0xff);
    CHECK(HistoryManager::checkpointContainingLedger(193, app->getConfig()) ==
          0xff);
    CHECK(HistoryManager::checkpointContainingLedger(194, app->getConfig()) ==
          0xff);
    // ...
    CHECK(HistoryManager::checkpointContainingLedger(253, app->getConfig()) ==
          0xff);
    CHECK(HistoryManager::checkpointContainingLedger(254, app->getConfig()) ==
          0xff);
    CHECK(HistoryManager::checkpointContainingLedger(255, app->getConfig()) ==
          0xff);
    CHECK(HistoryManager::checkpointContainingLedger(256, app->getConfig()) ==
          0x13f);
    CHECK(HistoryManager::checkpointContainingLedger(257, app->getConfig()) ==
          0x13f);
    CHECK(HistoryManager::checkpointContainingLedger(258, app->getConfig()) ==
          0x13f);
}

TEST_CASE("HistoryManager compress", "[history]")
{
    CatchupSimulation catchupSimulation{};

    std::string s = "hello there";
    HistoryManager& hm = catchupSimulation.getApp().getHistoryManager();
    std::string fname = hm.localFilename("compressme");
    {
        std::ofstream out;
        out.exceptions(std::ios::failbit | std::ios::badbit);
        out.open(fname, std::ofstream::binary);
        out.write(s.data(), s.size());
    }
    std::string compressed = fname + ".gz";
    auto& wm = catchupSimulation.getApp().getWorkScheduler();
    auto g = wm.executeWork<GzipFileWork>(fname);
    REQUIRE(g->getState() == BasicWork::State::WORK_SUCCESS);
    REQUIRE(!fs::exists(fname));
    REQUIRE(fs::exists(compressed));

    auto u = wm.executeWork<GunzipFileWork>(compressed);
    REQUIRE(u->getState() == BasicWork::State::WORK_SUCCESS);
    REQUIRE(fs::exists(fname));
    REQUIRE(!fs::exists(compressed));
}

TEST_CASE("HistoryArchiveState get_put", "[history]")
{
    CatchupSimulation catchupSimulation{};

    HistoryArchiveState has;
    has.currentLedger = 0x1234;

    auto archive =
        catchupSimulation.getApp().getHistoryArchiveManager().getHistoryArchive(
            catchupSimulation.getHistoryConfigurator().getArchiveDirName());
    REQUIRE(archive);

    has.resolveAllFutures();

    auto& wm = catchupSimulation.getApp().getWorkScheduler();
    auto put = wm.executeWork<PutHistoryArchiveStateWork>(has, archive);
    REQUIRE(put->getState() == BasicWork::State::WORK_SUCCESS);

    auto get = wm.executeWork<GetHistoryArchiveStateWork>(0, archive);
    REQUIRE(get->getState() == BasicWork::State::WORK_SUCCESS);
    HistoryArchiveState has2 = get->getHistoryArchiveState();
    REQUIRE(has2.currentLedger == 0x1234);
}

TEST_CASE("History bucket verification", "[history][catchup]")
{
    /* Tests bucket verification stage of catchup. Assumes ledger chain
     * verification was successful. **/

    Config cfg(getTestConfig());
    VirtualClock clock;
    auto cg = std::make_shared<TmpDirHistoryConfigurator>();
    cg->configure(cfg, true);
    Application::pointer app = createTestApplication(clock, cfg);
    REQUIRE(app->getHistoryArchiveManager().initializeHistoryArchive(
        cg->getArchiveDirName()));

    auto bucketGenerator = TestBucketGenerator{
        *app, app->getHistoryArchiveManager().getHistoryArchive(
                  cg->getArchiveDirName())};
    std::vector<std::string> hashes;
    auto& wm = app->getWorkScheduler();
    std::map<std::string, std::shared_ptr<LiveBucket>> mBuckets;
    auto tmpDir =
        std::make_unique<TmpDir>(app->getTmpDirManager().tmpDir("bucket-test"));

    SECTION("successful download and verify")
    {
        hashes.push_back(bucketGenerator.generateBucket(
            TestBucketState::CONTENTS_AND_HASH_OK));
        hashes.push_back(bucketGenerator.generateBucket(
            TestBucketState::CONTENTS_AND_HASH_OK));
        auto verify =
            wm.executeWork<DownloadBucketsWork>(mBuckets, hashes, *tmpDir);
        REQUIRE(verify->getState() == BasicWork::State::WORK_SUCCESS);
    }
    SECTION("download fails file not found")
    {
        hashes.push_back(
            bucketGenerator.generateBucket(TestBucketState::FILE_NOT_UPLOADED));

        auto verify =
            wm.executeWork<DownloadBucketsWork>(mBuckets, hashes, *tmpDir);
        REQUIRE(verify->getState() == BasicWork::State::WORK_FAILURE);
    }
    SECTION("download succeeds but unzip fails")
    {
        hashes.push_back(bucketGenerator.generateBucket(
            TestBucketState::CORRUPTED_ZIPPED_FILE));

        auto verify =
            wm.executeWork<DownloadBucketsWork>(mBuckets, hashes, *tmpDir);
        REQUIRE(verify->getState() == BasicWork::State::WORK_FAILURE);
    }
    SECTION("verify fails hash mismatch")
    {
        hashes.push_back(
            bucketGenerator.generateBucket(TestBucketState::HASH_MISMATCH));

        auto verify =
            wm.executeWork<DownloadBucketsWork>(mBuckets, hashes, *tmpDir);
        REQUIRE(verify->getState() == BasicWork::State::WORK_FAILURE);
    }
    SECTION("no hashes to verify")
    {
        // Ensure proper behavior when no hashes are passed in
        auto verify = wm.executeWork<DownloadBucketsWork>(
            mBuckets, std::vector<std::string>(), *tmpDir);
        REQUIRE(verify->getState() == BasicWork::State::WORK_SUCCESS);
    }
}

TEST_CASE("Ledger chain verification", "[ledgerheaderverification]")
{
    Config cfg(getTestConfig(0));
    VirtualClock clock;
    auto cg = std::make_shared<TmpDirHistoryConfigurator>();
    cg->configure(cfg, true);
    Application::pointer app = createTestApplication(clock, cfg);
    REQUIRE(app->getHistoryArchiveManager().initializeHistoryArchive(
        cg->getArchiveDirName()));

    auto tmpDir = app->getTmpDirManager().tmpDir("tmp-chain-test");
    auto& wm = app->getWorkScheduler();

    LedgerHeaderHistoryEntry firstVerified{};
    LedgerHeaderHistoryEntry verifiedAhead{};

    uint32_t initLedger = 127;
    auto ledgerRange = LedgerRange::inclusive(
        initLedger,
        initLedger +
            (HistoryManager::getCheckpointFrequency(app->getConfig()) * 10));
    CheckpointRange checkpointRange{ledgerRange, app->getHistoryManager()};
    auto ledgerChainGenerator = TestLedgerChainGenerator{
        *app,
        app->getHistoryArchiveManager().getHistoryArchive(
            cg->getArchiveDirName()),
        checkpointRange, tmpDir};

    auto checkExpectedBehavior = [&](Work::State expectedState,
                                     LedgerHeaderHistoryEntry lcl,
                                     LedgerHeaderHistoryEntry last,
                                     bool hasTrustedHash = true) {
        auto lclPair = LedgerNumHashPair(lcl.header.ledgerSeq,
                                         std::make_optional<Hash>(lcl.hash));
        auto ledgerRangeEnd = LedgerNumHashPair(
            last.header.ledgerSeq, hasTrustedHash
                                       ? std::make_optional<Hash>(last.hash)
                                       : std::nullopt);
        std::promise<LedgerNumHashPair> ledgerRangeEndPromise;
        std::shared_future<LedgerNumHashPair> ledgerRangeEndFuture =
            ledgerRangeEndPromise.get_future().share();
        ledgerRangeEndPromise.set_value(ledgerRangeEnd);

        auto fataFailurePromise = std::promise<bool>();
        std::shared_future<bool> fatalFailureFuture =
            fataFailurePromise.get_future().share();
        auto w = wm.executeWork<VerifyLedgerChainWork>(
            tmpDir, ledgerRange, lclPair, std::nullopt, ledgerRangeEndFuture,
            std::move(fataFailurePromise));
        REQUIRE(expectedState == w->getState());
        REQUIRE(fatalFailureFuture.valid());
        return fatalFailureFuture.get();
    };

    LedgerHeaderHistoryEntry lcl, last;
    SECTION("fully valid")
    {
        std::tie(lcl, last) = ledgerChainGenerator.makeLedgerChainFiles(
            HistoryManager::VERIFY_STATUS_OK);
        auto w =
            checkExpectedBehavior(BasicWork::State::WORK_SUCCESS, lcl, last);
        REQUIRE(!w);
    }
    SECTION("invalid link due to bad hash")
    {
        std::tie(lcl, last) = ledgerChainGenerator.makeLedgerChainFiles(
            HistoryManager::VERIFY_STATUS_ERR_BAD_HASH);
        auto w =
            checkExpectedBehavior(BasicWork::State::WORK_FAILURE, lcl, last);
        REQUIRE(!w);
    }
    SECTION("invalid ledger version without trusted hash")
    {
        std::tie(lcl, last) = ledgerChainGenerator.makeLedgerChainFiles(
            HistoryManager::VERIFY_STATUS_ERR_BAD_LEDGER_VERSION);
        auto w = checkExpectedBehavior(BasicWork::State::WORK_FAILURE, lcl,
                                       last, /* hasTrustedHash */ false);
        REQUIRE(!w);
    }
    SECTION("invalid ledger version with trusted hash")
    {
        std::tie(lcl, last) = ledgerChainGenerator.makeLedgerChainFiles(
            HistoryManager::VERIFY_STATUS_ERR_BAD_LEDGER_VERSION);
        auto w =
            checkExpectedBehavior(BasicWork::State::WORK_FAILURE, lcl, last);
        REQUIRE(w);
    }
    SECTION("overshot")
    {
        std::tie(lcl, last) = ledgerChainGenerator.makeLedgerChainFiles(
            HistoryManager::VERIFY_STATUS_ERR_OVERSHOT);
        auto w =
            checkExpectedBehavior(BasicWork::State::WORK_FAILURE, lcl, last);
        REQUIRE(!w);
    }
    SECTION("undershot")
    {
        std::tie(lcl, last) = ledgerChainGenerator.makeLedgerChainFiles(
            HistoryManager::VERIFY_STATUS_ERR_UNDERSHOT);
        auto w =
            checkExpectedBehavior(BasicWork::State::WORK_FAILURE, lcl, last);
        REQUIRE(!w);
    }
    SECTION("missing ledger files")
    {
        std::tie(lcl, last) = ledgerChainGenerator.makeLedgerChainFiles(
            HistoryManager::VERIFY_STATUS_ERR_MISSING_ENTRIES);
        auto w =
            checkExpectedBehavior(BasicWork::State::WORK_FAILURE, lcl, last);
        REQUIRE(!w);
    }
    SECTION("chain does not agree with LCL with trusted hash")
    {
        std::tie(lcl, last) = ledgerChainGenerator.makeLedgerChainFiles(
            HistoryManager::VERIFY_STATUS_OK);
        lcl.hash = HashUtils::pseudoRandomForTesting();
        auto w =
            checkExpectedBehavior(BasicWork::State::WORK_FAILURE, lcl, last);
        REQUIRE(w);
    }
    SECTION("chain does not agree with LCL without trusted hash")
    {
        std::tie(lcl, last) = ledgerChainGenerator.makeLedgerChainFiles(
            HistoryManager::VERIFY_STATUS_OK);
        lcl.hash = HashUtils::pseudoRandomForTesting();
        auto w = checkExpectedBehavior(BasicWork::State::WORK_FAILURE, lcl,
                                       last, false);
        REQUIRE(!w);
    }
    SECTION("chain does not agree with LCL on checkpoint boundary")
    {
        std::tie(lcl, last) = ledgerChainGenerator.makeLedgerChainFiles(
            HistoryManager::VERIFY_STATUS_OK);
        lcl.header.ledgerSeq +=
            HistoryManager::getCheckpointFrequency(app->getConfig()) - 1;
        lcl.hash = HashUtils::pseudoRandomForTesting();
        auto w =
            checkExpectedBehavior(BasicWork::State::WORK_FAILURE, lcl, last);
        REQUIRE(w);
    }
    SECTION("chain does not agree with LCL outside of range")
    {
        std::tie(lcl, last) = ledgerChainGenerator.makeLedgerChainFiles(
            HistoryManager::VERIFY_STATUS_OK);
        lcl.header.ledgerSeq -= 1;
        lcl.hash = HashUtils::pseudoRandomForTesting();
        auto w =
            checkExpectedBehavior(BasicWork::State::WORK_FAILURE, lcl, last);
        REQUIRE(w);
    }
    SECTION("chain does not agree with trusted hash")
    {
        std::tie(lcl, last) = ledgerChainGenerator.makeLedgerChainFiles(
            HistoryManager::VERIFY_STATUS_OK);
        last.hash = HashUtils::pseudoRandomForTesting();
        auto w =
            checkExpectedBehavior(BasicWork::State::WORK_FAILURE, lcl, last);
        REQUIRE(!w);
    }
    SECTION("missing file")
    {
        std::tie(lcl, last) = ledgerChainGenerator.makeLedgerChainFiles(
            HistoryManager::VERIFY_STATUS_OK);
        FileTransferInfo ft(tmpDir, FileType::HISTORY_FILE_TYPE_LEDGER,
                            last.header.ledgerSeq);
        std::remove(ft.localPath_nogz().c_str());

        // No crash
        auto w =
            checkExpectedBehavior(BasicWork::State::WORK_FAILURE, lcl, last);
        REQUIRE(!w);
    }
}

TEST_CASE("Tx results verification", "[batching][resultsverification]")
{
    CatchupSimulation catchupSimulation{};
    auto checkpointLedger = catchupSimulation.getLastCheckpointLedger(2);
    catchupSimulation.ensureOfflineCatchupPossible(checkpointLedger);

    auto tmpDir =
        catchupSimulation.getApp().getTmpDirManager().tmpDir("tx-results-test");
    auto& wm = catchupSimulation.getApp().getWorkScheduler();
    CheckpointRange range{LedgerRange::inclusive(1, checkpointLedger),
                          catchupSimulation.getApp().getHistoryManager()};

    auto verifyHeadersWork = wm.executeWork<BatchDownloadWork>(
        range, FileType::HISTORY_FILE_TYPE_LEDGER, tmpDir);
    REQUIRE(verifyHeadersWork->getState() == BasicWork::State::WORK_SUCCESS);
    SECTION("basic")
    {
        auto verify =
            wm.executeWork<DownloadVerifyTxResultsWork>(range, tmpDir);
        REQUIRE(verify->getState() == BasicWork::State::WORK_SUCCESS);
    }
    SECTION("header file missing")
    {
        FileTransferInfo ft(tmpDir, FileType::HISTORY_FILE_TYPE_LEDGER,
                            range.last());
        std::remove(ft.localPath_nogz().c_str());
        auto verify =
            wm.executeWork<DownloadVerifyTxResultsWork>(range, tmpDir);
        REQUIRE(verify->getState() == BasicWork::State::WORK_FAILURE);
    }
    SECTION("hash mismatch")
    {
        FileTransferInfo ft(tmpDir, FileType::HISTORY_FILE_TYPE_LEDGER,
                            range.last());
        XDRInputFileStream res;
        res.open(ft.localPath_nogz());
        std::vector<LedgerHeaderHistoryEntry> entries;
        LedgerHeaderHistoryEntry curr;
        while (res && res.readOne(curr))
        {
            entries.push_back(curr);
        }
        res.close();
        REQUIRE_FALSE(entries.empty());
        auto& lastEntry = entries.at(entries.size() - 1);
        lastEntry.header.txSetResultHash = HashUtils::pseudoRandomForTesting();
        std::remove(ft.localPath_nogz().c_str());

        XDROutputFileStream out(
            catchupSimulation.getApp().getClock().getIOContext(), true);
        out.open(ft.localPath_nogz());
        for (auto const& item : entries)
        {
            out.writeOne(item);
        }
        out.close();

        auto verify =
            wm.executeWork<DownloadVerifyTxResultsWork>(range, tmpDir);
        REQUIRE(verify->getState() == BasicWork::State::WORK_FAILURE);
    }
    SECTION("invalid result entries")
    {
        auto getResults = wm.executeWork<BatchDownloadWork>(
            range, FileType::HISTORY_FILE_TYPE_RESULTS, tmpDir);
        REQUIRE(getResults->getState() == BasicWork::State::WORK_SUCCESS);

        FileTransferInfo ft(tmpDir, FileType::HISTORY_FILE_TYPE_RESULTS,
                            range.last());
        XDRInputFileStream res;
        res.open(ft.localPath_nogz());
        std::vector<TransactionHistoryResultEntry> entries;
        TransactionHistoryResultEntry curr;
        while (res && res.readOne(curr))
        {
            entries.push_back(curr);
        }
        res.close();
        REQUIRE_FALSE(entries.empty());
        std::remove(ft.localPath_nogz().c_str());

        XDROutputFileStream out(
            catchupSimulation.getApp().getClock().getIOContext(), true);
        out.open(ft.localPath_nogz());
        // Duplicate entries
        for (size_t i = 0; i < entries.size(); ++i)
        {
            out.writeOne(entries[0]);
        }
        out.close();

        auto verify = wm.executeWork<VerifyTxResultsWork>(tmpDir, range.last());
        REQUIRE(verify->getState() == BasicWork::State::WORK_FAILURE);
    }
}

TEST_CASE("History publish", "[history][publish]")
{
    CatchupSimulation catchupSimulation{};
    auto checkpointLedger = catchupSimulation.getLastCheckpointLedger(1);
    catchupSimulation.ensureOfflineCatchupPossible(checkpointLedger);
}

void
validateCheckpointFiles(Application& app, uint32_t ledger, bool isFinalized)
{
    auto const& cfg = app.getConfig();
    auto validateHdr = [](std::string path, uint32_t ledger) {
        XDRInputFileStream hdrIn;
        hdrIn.open(path);
        LedgerHeaderHistoryEntry entry;
        while (hdrIn && hdrIn.readOne(entry))
        {
            REQUIRE(entry.header.ledgerSeq <= ledger);
        }
        REQUIRE(entry.header.ledgerSeq == ledger);
    };

    auto checkpoint = HistoryManager::checkpointContainingLedger(ledger, cfg);
    FileTransferInfo res(FileType::HISTORY_FILE_TYPE_RESULTS, checkpoint, cfg);
    FileTransferInfo txs(FileType::HISTORY_FILE_TYPE_TRANSACTIONS, checkpoint,
                         cfg);
    FileTransferInfo headers(FileType::HISTORY_FILE_TYPE_LEDGER, checkpoint,
                             cfg);
    if (isFinalized)
    {
        REQUIRE(fs::exists(res.localPath_nogz()));
        REQUIRE(fs::exists(txs.localPath_nogz()));
        REQUIRE(!fs::exists(res.localPath_nogz_dirty()));
        REQUIRE(!fs::exists(txs.localPath_nogz_dirty()));
        REQUIRE(!fs::exists(headers.localPath_nogz_dirty()));
        validateHdr(headers.localPath_nogz(), ledger);
    }
    else
    {
        REQUIRE(!fs::exists(res.localPath_nogz()));
        REQUIRE(!fs::exists(txs.localPath_nogz()));
        REQUIRE(!fs::exists(headers.localPath_nogz()));
        REQUIRE(fs::exists(res.localPath_nogz_dirty()));
        REQUIRE(fs::exists(txs.localPath_nogz_dirty()));
        validateHdr(headers.localPath_nogz_dirty(), ledger);
    }
}

TEST_CASE("History publish with restart", "[history][publish]")
{
    auto catchupSimulation =
        CatchupSimulation(VirtualClock::VIRTUAL_TIME,
                          std::make_shared<TmpDirHistoryConfigurator>(), true,
                          Config::TESTDB_BUCKET_DB_PERSISTENT);
    auto checkpointLedger = catchupSimulation.getLastCheckpointLedger(2);

    // Restart at various points in the checkpoint, core should continue
    // properly writing checkpoint files
    auto ledgerNums = std::vector<uint32_t>{
        LedgerManager::GENESIS_LEDGER_SEQ,
        10,
        catchupSimulation.getLastCheckpointLedger(1),
        HistoryManager::firstLedgerInCheckpointContaining(
            checkpointLedger, catchupSimulation.getApp().getConfig()),
        checkpointLedger - 1,
        checkpointLedger};
    for (auto ledgerNum : ledgerNums)
    {
        SECTION("Restart at ledger " + std::to_string(ledgerNum))
        {
            SECTION("graceful")
            {
                catchupSimulation.ensureOfflineCatchupPossible(checkpointLedger,
                                                               ledgerNum);
            }
            SECTION("crash leaves dirty data")
            {
                if (ledgerNum == LedgerManager::GENESIS_LEDGER_SEQ)
                {
                    // Genesis ledger is established when the app is created
                    continue;
                }
                auto& hm = static_cast<HistoryManagerImpl&>(
                    catchupSimulation.getApp().getHistoryManager());
                hm.mThrowOnAppend = ledgerNum;
                REQUIRE_THROWS_AS(
                    catchupSimulation.ensureOfflineCatchupPossible(
                        checkpointLedger),
                    std::runtime_error);
                // Before proceeding, ensure files are actually corrupt
                validateCheckpointFiles(catchupSimulation.getApp(), ledgerNum,
                                        false);
                // Restart app, truncate dirty data in checkpoints, proceed to
                // publish
                catchupSimulation.restartApp();
                catchupSimulation.ensureOfflineCatchupPossible(
                    checkpointLedger);
            }

            // Now catchup to ensure published checkpoints are valid
            auto app = catchupSimulation.createCatchupApplication(
                std::numeric_limits<uint32_t>::max(),
                Config::TESTDB_BUCKET_DB_PERSISTENT, "app");
            REQUIRE(catchupSimulation.catchupOffline(app, checkpointLedger));
        }
    }
}

TEST_CASE("History publish to multiple archives", "[history]")
{
    Config cfg(getTestConfig());
    VirtualClock clock;
    auto cg =
        std::make_shared<MultiArchiveHistoryConfigurator>(/* numArchives */ 3);
    CatchupSimulation catchupSimulation{VirtualClock::VIRTUAL_TIME, cg, false};

    auto& app = catchupSimulation.getApp();
    for (auto const& cfgtor : cg->getConfigurators())
    {
        CHECK(app.getHistoryArchiveManager().initializeHistoryArchive(
            cfgtor->getArchiveDirName()));
    }

    app.start();
    auto checkpointLedger = catchupSimulation.getLastCheckpointLedger(2);
    catchupSimulation.ensureOfflineCatchupPossible(checkpointLedger);

    auto catchupApp = catchupSimulation.createCatchupApplication(
        64, Config::TESTDB_BUCKET_DB_PERSISTENT, "app");

    // Actually perform catchup and make sure everything is correct
    REQUIRE(catchupSimulation.catchupOffline(catchupApp, checkpointLedger));
}

TEST_CASE("History catchup with extra validation", "[history][publish]")
{
    CatchupSimulation catchupSimulation{};
    auto checkpointLedger = catchupSimulation.getLastCheckpointLedger(3);
    catchupSimulation.ensureOfflineCatchupPossible(checkpointLedger);

    auto app = catchupSimulation.createCatchupApplication(
        std::numeric_limits<uint32_t>::max(),
        Config::TESTDB_BUCKET_DB_PERSISTENT, "app");
    REQUIRE(catchupSimulation.catchupOffline(app, checkpointLedger, true));
}

TEST_CASE("Publish works correctly post shadow removal", "[history]")
{
    // Given a HAS, verify that appropriate levels have "next" cleared, while
    // the remaining initialized levels have output hashes.
    auto checkFuture = [](uint32_t maxLevelCleared,
                          uint32_t maxLevelInitialized,
                          HistoryArchiveState const& has) {
        REQUIRE(maxLevelCleared <= maxLevelInitialized);
        for (uint32_t i = 0; i <= maxLevelInitialized; ++i)
        {
            auto next = has.currentBuckets[i].next;
            if (i <= maxLevelCleared)
            {
                REQUIRE(next.isClear());
            }
            else
            {
                REQUIRE(next.hasOutputHash());
            }
        }
    };

    auto verifyFutureBucketsInHAS = [&](CatchupSimulation& sim,
                                        uint32_t upgradeLedger,
                                        uint32_t expectedLevelsCleared) {
        // Perform publish: 2 checkpoints (or 127 ledgers) correspond to 3
        // levels being initialized and partially filled in the bucketlist
        sim.setUpgradeLedger(upgradeLedger,
                             LiveBucket::FIRST_PROTOCOL_SHADOWS_REMOVED);
        auto checkpointLedger = sim.getLastCheckpointLedger(2);
        auto maxLevelTouched = 3;
        sim.ensureOfflineCatchupPossible(checkpointLedger);

        auto& app = sim.getApp();
        auto w =
            app.getWorkScheduler().executeWork<GetHistoryArchiveStateWork>();
        auto has = w->getHistoryArchiveState();
        REQUIRE(w->getState() == BasicWork::State::WORK_SUCCESS);
        checkFuture(expectedLevelsCleared, maxLevelTouched, has);
    };

    auto configurator =
        std::make_shared<RealGenesisTmpDirHistoryConfigurator>();
    CatchupSimulation catchupSimulation{VirtualClock::VIRTUAL_TIME,
                                        configurator};

    uint32_t oldProto =
        static_cast<uint32_t>(LiveBucket::FIRST_PROTOCOL_SHADOWS_REMOVED) - 1;
    catchupSimulation.generateRandomLedger(oldProto);

    // The next sections reflect how future buckets in HAS change, depending on
    // the protocol version of the merge. Intuitively, if an upgrade is done
    // closer to publish, less levels had time to start a new-style merge,
    // meaning that some levels will still have an output in them.
    SECTION("upgrade way before publish")
    {
        // Upgrade happened early enough to allow new-style buckets to propagate
        // down to level 2 snap, so, that all levels up to 3 are performing new
        // style merges.
        uint32_t upgradeLedger = 64;
        verifyFutureBucketsInHAS(catchupSimulation, upgradeLedger, 3);
    }
    SECTION("upgrade slightly later")
    {
        // Between ledger 80 and 127, there is not enough ledgers to propagate
        // new-style bucket to level 2 snap, so level 3 still performs an
        // old-style merge, while all levels above perform new style merges.
        uint32_t upgradeLedger = 80;
        verifyFutureBucketsInHAS(catchupSimulation, upgradeLedger, 2);
    }
    SECTION("upgrade close to publish")
    {
        // At upgrade ledger 125, level0Curr is new-style. Then, at ledger 126,
        // a new-style merge for level 1 is started (lev0Snap is new-style, so
        // level 0 and 1 should be clear
        uint32_t upgradeLedger = 125;
        verifyFutureBucketsInHAS(catchupSimulation, upgradeLedger, 1);
    }
    SECTION("upgrade right before publish")
    {
        // At ledger 127, only level0Curr is of new version, so all levels below
        // are left as-is.
        uint32_t upgradeLedger = 127;
        verifyFutureBucketsInHAS(catchupSimulation, upgradeLedger, 0);
    }
}

static std::string
resumeModeName(uint32_t count)
{
    switch (count)
    {
    case 0:
        return "CATCHUP_MINIMAL";
    case std::numeric_limits<uint32_t>::max():
        return "CATCHUP_COMPLETE";
    default:
        return "CATCHUP_RECENT";
    }
}

static std::string
dbModeName(Config::TestDbMode mode)
{
    switch (mode)
    {
    case Config::TESTDB_IN_MEMORY:
        return "TESTDB_IN_MEMORY";
#ifdef USE_POSTGRES
    case Config::TESTDB_POSTGRESQL:
        return "TESTDB_POSTGRESQL";
#endif
    case Config::TESTDB_BUCKET_DB_PERSISTENT:
        return "TESTDB_BUCKET_DB_PERSISTENT";
    case Config::TESTDB_BUCKET_DB_VOLATILE:
        return "TESTDB_BUCKET_DB_VOLATILE";
    case Config::TESTDB_DEFAULT:
        return "TESTDB_DEFAULT";
    default:
        abort();
    }
}

TEST_CASE("History catchup", "[history][catchup][acceptance]")
{
    // needs REAL_TIME here, as resolve-snapshot works will fail for one of the
    // sections again and again - as it is set to RETRY_FOREVER it can generate
    // megabytes of unnecessary log entries
    CatchupSimulation catchupSimulation{VirtualClock::REAL_TIME};
    auto checkpointLedger = catchupSimulation.getLastCheckpointLedger(3);
    auto app = catchupSimulation.createCatchupApplication(
        std::numeric_limits<uint32_t>::max(),
        Config::TESTDB_BUCKET_DB_PERSISTENT, "app");

    auto offlineNonCheckpointDestinationLedger =
        checkpointLedger -
        HistoryManager::getCheckpointFrequency(app->getConfig()) / 2;

    SECTION("when not enough publishes has been performed")
    {
        // only 2 first checkpoints can be published in this section
        catchupSimulation.ensureLedgerAvailable(checkpointLedger);

        SECTION("online")
        {
            REQUIRE(!catchupSimulation.catchupOnline(app, checkpointLedger));
        }

        SECTION("offline")
        {
            REQUIRE(!catchupSimulation.catchupOffline(app, checkpointLedger));
        }

        SECTION("offline, in the middle of checkpoint")
        {
            REQUIRE(!catchupSimulation.catchupOffline(
                app, offlineNonCheckpointDestinationLedger));
        }
    }

    SECTION("when enough publishes has been performed, but no trigger ledger "
            "was externalized")
    {
        // 1 ledger is for publish-trigger
        catchupSimulation.ensureLedgerAvailable(checkpointLedger + 1);
        catchupSimulation.ensurePublishesComplete();

        SECTION("online")
        {
            REQUIRE(!catchupSimulation.catchupOnline(app, checkpointLedger));
        }

        SECTION("offline")
        {
            REQUIRE(catchupSimulation.catchupOffline(app, checkpointLedger));
        }

        SECTION("offline, in the middle of checkpoint")
        {
            REQUIRE(catchupSimulation.catchupOffline(
                app, offlineNonCheckpointDestinationLedger));
        }
    }

    SECTION("when enough publishes has been performed, but no closing ledger "
            "was externalized")
    {
        // 1 ledger is for publish-trigger, 1 ledger is catchup-trigger ledger
        catchupSimulation.ensureLedgerAvailable(checkpointLedger + 2);
        catchupSimulation.ensurePublishesComplete();
        REQUIRE(catchupSimulation.catchupOnline(app, checkpointLedger));
    }

    SECTION("when enough publishes has been performed, 3 ledgers are buffered "
            "and no closing ledger was externalized")
    {
        // 1 ledger is for publish-trigger, 1 ledger is catchup-trigger ledger,
        // 3 ledgers are buffered
        catchupSimulation.ensureLedgerAvailable(checkpointLedger + 5);
        catchupSimulation.ensurePublishesComplete();
        REQUIRE(catchupSimulation.catchupOnline(app, checkpointLedger, 3));
    }

    SECTION("when enough publishes has been performed, 3 ledgers are buffered "
            "and closing ledger was externalized")
    {
        // 1 ledger is for publish-trigger, 1 ledger is catchup-trigger ledger,
        // 3 ledgers are buffered, 1 ledger is closing
        catchupSimulation.ensureLedgerAvailable(checkpointLedger + 6);
        catchupSimulation.ensurePublishesComplete();
        REQUIRE(catchupSimulation.catchupOnline(app, checkpointLedger, 3));
    }
}

TEST_CASE("Publish throttles catchup", "[history][catchup][acceptance]")
{
    CatchupSimulation catchupSimulation{};
    auto checkpointLedger = catchupSimulation.getLastCheckpointLedger(20);
    catchupSimulation.ensureLedgerAvailable(checkpointLedger + 1);
    catchupSimulation.ensurePublishesComplete();
    auto app = catchupSimulation.createCatchupApplication(
        std::numeric_limits<uint32_t>::max(), Config::TESTDB_DEFAULT, "app",
        /* publish */ true);
    REQUIRE(catchupSimulation.catchupOffline(app, checkpointLedger));
}

TEST_CASE("History catchup with different modes",
          "[history][catchup][acceptance]")
{
    CatchupSimulation catchupSimulation{};

    auto checkpointLedger = catchupSimulation.getLastCheckpointLedger(3);
    catchupSimulation.ensureOnlineCatchupPossible(checkpointLedger, 5);

    std::vector<Application::pointer> apps;

    std::vector<uint32_t> counts = {0, std::numeric_limits<uint32_t>::max(),
                                    60};

    std::vector<Config::TestDbMode> dbModes = {
        Config::TESTDB_BUCKET_DB_PERSISTENT};
#ifdef USE_POSTGRES
    if (!force_sqlite)
        dbModes.push_back(Config::TESTDB_POSTGRESQL);
#endif

    for (auto dbMode : dbModes)
    {
        for (auto count : counts)
        {
            auto a = catchupSimulation.createCatchupApplication(
                count, dbMode,
                std::string("full, ") + resumeModeName(count) + ", " +
                    dbModeName(dbMode));
            REQUIRE(catchupSimulation.catchupOnline(a, checkpointLedger, 5));
            apps.push_back(a);
        }
    }
}

TEST_CASE("Retriggering catchups after trimming mSyncingLedgers",
          "[history][catchup]")
{
    CatchupSimulation catchupSimulation{};

    // Try to get to ledger 1000 with 200 ledgers buffered
    // when there are 20 checkpoints available.
    // In order to prevent mSyncingLedgers from growing indefinitely,
    // it gets trimmed periodically.
    // This test makes sure that we are able to catch up to the network
    // even when some ledgers get trimmed from mSyncingLedgers.

    const auto bufferLedgers = 200;
    const auto initLedger = 1000;
    const auto numCheckpointsAvailable = 20;

    auto checkpointLedger =
        catchupSimulation.getLastCheckpointLedger(numCheckpointsAvailable);
    catchupSimulation.ensureOnlineCatchupPossible(checkpointLedger);

    auto app = catchupSimulation.createCatchupApplication(
        std::numeric_limits<uint32_t>::max(), Config::TESTDB_DEFAULT,
        std::string("Retriggering catchups after trimming mSyncingLedgers"));
    auto& lm = app->getLedgerManager();

    auto& herder = static_cast<HerderImpl&>(app->getHerder());

    auto runCatchup = [&](uint32_t expectedDestination) {
        auto startCatchupMetrics =
            app->getLedgerApplyManager().getCatchupMetrics();

        auto expectedCatchupWork =
            catchupSimulation.computeCatchupPerformedWork(
                lm.getLastClosedLedgerNum(),
                CatchupConfiguration{expectedDestination,
                                     app->getConfig().CATCHUP_RECENT,
                                     CatchupConfiguration::Mode::ONLINE},
                *app);

        testutil::crankUntil(
            app,
            [&]() { return app->getLedgerApplyManager().catchupWorkIsDone(); },
            std::chrono::seconds{
                std::max<int64>(expectedCatchupWork.mTxSetsApplied + 15, 60)});

        auto endCatchupMetrics =
            app->getLedgerApplyManager().getCatchupMetrics();
        auto catchupPerformedWork =
            CatchupPerformedWork{endCatchupMetrics - startCatchupMetrics};

        REQUIRE(catchupPerformedWork == expectedCatchupWork);
    };

    // Externalize (to the catchup LM) the range of ledgers between initLedger
    // and as near as we can get to the first ledger of the block after
    // initLedger (inclusive), so that there's something to knit-up with. Do not
    // externalize anything we haven't yet published, of course.
    const uint32_t firstLedgerInCheckpoint =
        HistoryManager::firstLedgerAfterCheckpointContaining(initLedger,
                                                             app->getConfig());

    const uint32_t triggerLedger = HistoryManager::ledgerToTriggerCatchup(
        firstLedgerInCheckpoint, app->getConfig());

    // 1. The app hears initLedger, ..., dividingLedger - 1.
    //    NB: dividingLedger must be chosen such that mSyncingLedgers gets
    //    trimmed in this step.
    // 2. We will let Catchup run right after hearing (dividingLedger - 1).
    // 3. The app hears dividingLedger, ..., triggerLedger + bufferLedgers.
    // 4. We will let Catchup run again.
    auto runTest = [&](uint32_t const dividingLedger) {
        // The app hears initLedger, ..., dividingLedger - 1.
        for (uint32_t n = initLedger; n < dividingLedger; ++n)
        {
            catchupSimulation.externalizeLedger(herder, n);
        }

        // Let the catchup run.
        // We expect that it'll land at the checkpoint containing (initLedger -
        // 1). It can't apply the buffered ledgers because mSyncingLedgers must
        // have popped some elements in order to prevent it from growing
        // exponentially, and thus there is a gap between the LCL and
        // mSyncingLedgers.
        runCatchup(HistoryManager::checkpointContainingLedger(
            initLedger - 1, app->getConfig()));

        // As mentioned above, mSyncingLedgers must have been trimmed
        // after hearing up to (dividingLedger - 1).
        // And if it has been trimmed, CatchupWork should not be able to catch
        // up without being re-triggered.
        REQUIRE(!lm.isSynced());

        // In order to close the gap between the LCL and mSyncingLedgers,
        // we need to run a CatchupWork again.
        // processLedger is responsible for that, and thus we call it
        // by externalizing ledgers.
        for (uint32_t n = dividingLedger; n <= triggerLedger + bufferLedgers;
             n++)
        {
            catchupSimulation.externalizeLedger(herder, n);
        }

        runCatchup(HistoryManager::lastLedgerBeforeCheckpointContaining(
            dividingLedger, app->getConfig()));

        REQUIRE(lm.getLastClosedLedgerNum() == triggerLedger + bufferLedgers);
        catchupSimulation.externalizeLedger(herder,
                                            triggerLedger + bufferLedgers + 1);

        testutil::crankUntil(
            app,
            [&]() {
                return lm.getLastClosedLedgerNum() ==
                       triggerLedger + bufferLedgers + 1;
            },
            std::chrono::seconds{60});

        REQUIRE(lm.isSynced());

        REQUIRE(lm.getLastClosedLedgerNum() ==
                triggerLedger + bufferLedgers + 1);

        catchupSimulation.validateCatchup(app);
    };

    SECTION("Catchup runs twice to catch up")
    {
        auto dividingLedger = triggerLedger + bufferLedgers;
        runTest(dividingLedger);
    }

    SECTION(
        "mSyncingLedgers gets trimmed between the first and second Catchup run")
    {
        // When adding the second ledger in a checkpoint,
        // we become certain that nodes started publishing the checkpoint.
        // Thus the ledgers before the checkpoint will be trimmed.
        // By setting dividingLedger to the second ledger in a checkpoint,
        // we can make sure that mSyncingLedgers gets trimmed between the first
        // and second Catchup run.
        auto dividingLedger =
            HistoryManager::firstLedgerInCheckpointContaining(
                triggerLedger + bufferLedgers, app->getConfig()) +
            1;
        runTest(dividingLedger);
    }
}

TEST_CASE("History prefix catchup", "[history][catchup]")
{
    CatchupSimulation catchupSimulation{};

    auto checkpointLedger = catchupSimulation.getLastCheckpointLedger(3);
    catchupSimulation.ensureOnlineCatchupPossible(checkpointLedger, 5);

    auto a = catchupSimulation.createCatchupApplication(
        std::numeric_limits<uint32_t>::max(), Config::TESTDB_DEFAULT,
        std::string("Catchup to prefix of published history"));
    // Try to catchup to ledger 10, which is part of first checkpoint (ending
    // at 63), witch 5 buffered ledgers. It will succeed (as 3 checkpoints are
    // available in history) and it will land on ledger 64 + 7 = 71.
    // Externalizing ledger 65 triggers catchup (as only at this point we can
    // be sure that publishing history up to ledger 63 has started), then we
    // simulate 5 buffered ledgers and at last we need one closing ledger to
    // get us into synced state.
    REQUIRE(catchupSimulation.catchupOnline(a, 10, 5));
    uint32_t freq = HistoryManager::getCheckpointFrequency(a->getConfig());
    REQUIRE(a->getLedgerManager().getLastClosedLedgerNum() == freq + 7);

    // Try to catchup to ledger 74, which is part of second checkpoint (ending
    // at 127), witch 5 buffered ledgers. It will succeed (as 3 checkpoints are
    // available in history) and it will land on ledger 128 + 7 = 135.
    // Externalizing ledger 129 triggers catchup (as only at this point we can
    // be sure that publishing history up to ledger 127 has started), then we
    // simulate 5 buffered ledgers and at last we need one closing ledger to
    // get us into synced state.
    auto b = catchupSimulation.createCatchupApplication(
        std::numeric_limits<uint32_t>::max(), Config::TESTDB_DEFAULT,
        std::string("Catchup to second prefix of published history"));
    REQUIRE(catchupSimulation.catchupOnline(b, freq + 10, 5));
    REQUIRE(b->getLedgerManager().getLastClosedLedgerNum() == 2 * freq + 7);
}

TEST_CASE("Catchup with protocol upgrade", "[catchup][history]")
{
    auto testUpgrade = [&](ProtocolVersion upgradeVersion) {
        uint32_t oldVersion = static_cast<uint32_t>(upgradeVersion) - 1;

        auto configurator =
            std::make_shared<RealGenesisTmpDirHistoryConfigurator>();
        CatchupSimulation catchupSimulation{VirtualClock::VIRTUAL_TIME,
                                            configurator};
        REQUIRE(catchupSimulation.getApp()
                    .getLedgerManager()
                    .getLastClosedLedgerHeader()
                    .header.ledgerVersion == 0);
        catchupSimulation.generateRandomLedger(oldVersion);
        std::vector<uint32_t> catchupLedgers = {
            0, std::numeric_limits<uint32_t>::max(), 32, 60};
        auto executeUpgrade = [&](uint32_t upgradeLedger) {
            catchupSimulation.setUpgradeLedger(upgradeLedger, upgradeVersion);
            auto checkpointLedger =
                catchupSimulation.getLastCheckpointLedger(3);
            catchupSimulation.ensureOnlineCatchupPossible(checkpointLedger);

            for (auto count : catchupLedgers)
            {
                auto a = catchupSimulation.createCatchupApplication(
                    count, Config::TESTDB_DEFAULT,
                    std::string("full, ") + resumeModeName(count) + ", " +
                        dbModeName(Config::TESTDB_DEFAULT));
                REQUIRE(a->getLedgerManager()
                            .getLastClosedLedgerHeader()
                            .header.ledgerVersion == 0);
                REQUIRE(catchupSimulation.catchupOnline(a, checkpointLedger));
            }
        };

        SECTION("Upgrade at checkpoint start")
        {
            executeUpgrade(64);
        }
        SECTION("Upgrade right after checkpoint start")
        {
            executeUpgrade(65);
        }

        SECTION("Upgrade mid-checkpoint")
        {
            executeUpgrade(80);
        }

        SECTION("Upgrade at checkpoint end")
        {
            executeUpgrade(127);
        }
    };
    SECTION("generalized tx set upgrade")
    {
        if (protocolVersionEquals(Config::CURRENT_LEDGER_PROTOCOL_VERSION,
                                  SOROBAN_PROTOCOL_VERSION))
        {
            testUpgrade(SOROBAN_PROTOCOL_VERSION);
        }
    }
}

TEST_CASE("Catchup fatal failure", "[catchup][history]")
{
    CatchupSimulation catchupSimulation{};

    auto checkpointLedger = catchupSimulation.getLastCheckpointLedger(3);
    catchupSimulation.ensureOnlineCatchupPossible(checkpointLedger, 5);

    auto a = catchupSimulation.createCatchupApplication(
        std::numeric_limits<uint32_t>::max(), Config::TESTDB_DEFAULT,
        std::string("Catchup retry"), false,
        catchupSimulation.getApp().getConfig().LEDGER_PROTOCOL_VERSION - 1);

    REQUIRE(!catchupSimulation.catchupOnline(a, 10, 5));
    auto& lam =
        static_cast<LedgerApplyManagerImpl&>(a->getLedgerApplyManager());
    REQUIRE(lam.getCatchupWork());
    REQUIRE(lam.getCatchupWork()->fatalFailure());

    // Bad ledger version is a fatal failure
    // Try catching up again, no catchup work should be started as we're in
    // fatal failure mode
    REQUIRE(!catchupSimulation.catchupOnline(a, 70, 5));
    REQUIRE(lam.getCatchupWork() == nullptr);
    REQUIRE(lam.getCatchupFatalFailure());
}

TEST_CASE("Catchup non-initentry buckets to initentry-supporting works",
          "[history][bucket][acceptance]")
{
    uint32_t newProto = static_cast<uint32_t>(
        LiveBucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY);
    uint32_t oldProto = newProto - 1;
    auto configurator =
        std::make_shared<RealGenesisTmpDirHistoryConfigurator>();
    CatchupSimulation catchupSimulation{VirtualClock::VIRTUAL_TIME,
                                        configurator};

    // Upgrade to oldProto
    catchupSimulation.generateRandomLedger(oldProto);

    auto checkpointLedger = catchupSimulation.getLastCheckpointLedger(3);
    catchupSimulation.ensureOnlineCatchupPossible(checkpointLedger);

    std::vector<Application::pointer> apps;
    std::vector<uint32_t> counts = {0, std::numeric_limits<uint32_t>::max(),
                                    60};
    for (auto count : counts)
    {
        auto a = catchupSimulation.createCatchupApplication(
            count, Config::TESTDB_DEFAULT,
            std::string("full, ") + resumeModeName(count) + ", " +
                dbModeName(Config::TESTDB_DEFAULT));
        REQUIRE(catchupSimulation.catchupOnline(a, checkpointLedger - 2));

        // Check that during catchup/replay, we did not use any INITENTRY code,
        // were still on the old protocol.
        auto mc = a->getBucketManager().readMergeCounters();
        REQUIRE(mc.mPostInitEntryProtocolMerges == 0);
        REQUIRE(mc.mNewInitEntries == 0);
        REQUIRE(mc.mOldInitEntries == 0);

        // Now that a is caught up, start advancing it at catchup point.
        for (auto i = 0; i < 3; ++i)
        {
            auto root = TestAccount{*a, txtest::getRoot(a->getNetworkID())};
            auto stranger = TestAccount{
                *a, txtest::getAccount(fmt::format("stranger{}", i))};
            auto& lm = a->getLedgerManager();
            uint32_t ledgerSeq = lm.getLastClosedLedgerNum() + 1;
            uint64_t minBalance = lm.getLastMinBalance(5);
            uint64_t big = minBalance + ledgerSeq;
            uint64_t closeTime = 60 * 5 * ledgerSeq;

            auto [txSet, applicableTxSet] = makeTxSetFromTransactions(
                {root.tx({txtest::createAccount(stranger, big)})}, *a, 0, 0);

            // On first iteration of advance, perform a ledger-protocol version
            // upgrade to the new protocol, to activate INITENTRY behaviour.
            auto upgrades = xdr::xvector<UpgradeType, 6>{};
            if (i == 0)
            {
                auto ledgerUpgrade = LedgerUpgrade{LEDGER_UPGRADE_VERSION};
                ledgerUpgrade.newLedgerVersion() = newProto;
                auto v = xdr::xdr_to_opaque(ledgerUpgrade);
                upgrades.push_back(UpgradeType{v.begin(), v.end()});
            }
            CLOG_DEBUG(
                History, "Closing synthetic ledger {} with {} txs (txhash:{})",
                ledgerSeq,
                applicableTxSet->size(lm.getLastClosedLedgerHeader().header),
                hexAbbrev(txSet->getContentsHash()));
            StellarValue sv = a->getHerder().makeStellarValue(
                txSet->getContentsHash(), closeTime, upgrades,
                a->getConfig().NODE_SEED);
            lm.closeLedger(LedgerCloseData(ledgerSeq, txSet, sv));
        }

        // Check that we did in fact use INITENTRY code.
        mc = a->getBucketManager().readMergeCounters();
        REQUIRE(mc.mPostInitEntryProtocolMerges != 0);
        REQUIRE(mc.mNewInitEntries != 0);
        REQUIRE(mc.mOldInitEntries != 0);

        apps.push_back(a);
    }
}

TEST_CASE("Publish catchup alternation with stall",
          "[history][catchup][acceptance]")
{
    CatchupSimulation catchupSimulation{};
    auto& lm = catchupSimulation.getApp().getLedgerManager();

    // Publish in simulation, catch up in completeApp and minimalApp.
    // CompleteApp will catch up using CATCHUP_COMPLETE, minimalApp will use
    // CATCHUP_MINIMAL.
    int checkpoints = 3;
    auto checkpointLedger =
        catchupSimulation.getLastCheckpointLedger(checkpoints);
    catchupSimulation.ensureOnlineCatchupPossible(checkpointLedger, 5);

    auto completeApp = catchupSimulation.createCatchupApplication(
        std::numeric_limits<uint32_t>::max(), Config::TESTDB_DEFAULT,
        std::string("completeApp"));
    auto minimalApp = catchupSimulation.createCatchupApplication(
        0, Config::TESTDB_DEFAULT, std::string("minimalApp"));

    REQUIRE(catchupSimulation.catchupOnline(completeApp, checkpointLedger, 5));
    REQUIRE(catchupSimulation.catchupOnline(minimalApp, checkpointLedger, 5));

    for (int i = 1; i < 4; ++i)
    {
        // Now alternate between publishing new stuff and catching up to it.
        checkpoints += i;
        checkpointLedger =
            catchupSimulation.getLastCheckpointLedger(checkpoints);
        catchupSimulation.ensureOnlineCatchupPossible(checkpointLedger, 5);

        REQUIRE(
            catchupSimulation.catchupOnline(completeApp, checkpointLedger, 5));
        REQUIRE(
            catchupSimulation.catchupOnline(minimalApp, checkpointLedger, 5));
    }

    // Finally, publish a little more history than the last publish-point
    // but not enough to get to the _next_ publish-point:
    catchupSimulation.generateRandomLedger();
    catchupSimulation.generateRandomLedger();
    catchupSimulation.generateRandomLedger();

    // Attempting to catch up here should _stall_. We evaluate stalling
    // by executing 30 seconds of the event loop and assuming that failure
    // to catch up within that time means 'stalled'.
    auto targetLedger = lm.getLastClosedLedgerNum();
    REQUIRE(!catchupSimulation.catchupOnline(completeApp, targetLedger, 5));
    REQUIRE(!catchupSimulation.catchupOnline(minimalApp, targetLedger, 5));

    // Now complete this publish cycle and confirm that the stalled apps
    // will catch up.
    catchupSimulation.ensureOnlineCatchupPossible(targetLedger, 5);

    REQUIRE(catchupSimulation.catchupOnline(completeApp, targetLedger, 5));
    REQUIRE(catchupSimulation.catchupOnline(minimalApp, targetLedger, 5));
}

TEST_CASE("Publish catchup via s3", "[!hide][s3]")
{
    CatchupSimulation catchupSimulation{
        VirtualClock::VIRTUAL_TIME, std::make_shared<S3HistoryConfigurator>()};
    auto checkpointLedger = catchupSimulation.getLastCheckpointLedger(3);
    catchupSimulation.ensureOfflineCatchupPossible(checkpointLedger);

    auto app = catchupSimulation.createCatchupApplication(
        std::numeric_limits<uint32_t>::max(), Config::TESTDB_DEFAULT, "s3");
    REQUIRE(catchupSimulation.catchupOnline(app, checkpointLedger, 5));
}

TEST_CASE_VERSIONS(
    "HAS in publishqueue remains in pristine state until publish", "[history]")
{
    // In this test we generate some buckets and cause a checkpoint to be
    // published, checking that the (cached) set of buckets referenced by the
    // publish queue is/was equal to the set we'd expect from a
    // partially-resolved HAS. This test is unfortunately quite "aware" of the
    // internals of the subsystems it's touching; they don't really have
    // well-developed testing interfaces. It's also a bit sensitive to the
    // amount of work done in a single crank and the fact that we're stopping
    // the crank just before firing the callback that clears the cached set of
    // buckets referenced by the publish queue.

    Config cfg(getTestConfig(0));
    cfg.MANUAL_CLOSE = false;
    cfg.MAX_CONCURRENT_SUBPROCESSES = 0;
    TmpDirHistoryConfigurator tcfg;
    cfg = tcfg.configure(cfg, true);
    VirtualClock clock;

    BucketTestUtils::for_versions_with_differing_bucket_logic(
        cfg, [&](Config const& cfg) {
            Application::pointer app = createTestApplication(clock, cfg);
            auto& hm = app->getHistoryManager();
            auto& lm = app->getLedgerManager();
            auto& bl = app->getBucketManager().getLiveBucketList();

            while (hm.getPublishQueueCount() != 1)
            {
                auto lcl = lm.getLastClosedLedgerHeader();
                lcl.header.ledgerSeq += 1;
                BucketTestUtils::addLiveBatchAndUpdateSnapshot(
                    *app, lcl.header, {},
                    LedgerTestUtils::generateValidUniqueLedgerEntries(8), {});
                clock.crank(true);
            }

            // Capture publish queue's view of HAS right before taking snapshot
            auto queuedHAS =
                HistoryManager::getPublishQueueStates(app->getConfig())[0];

            // Now take snapshot and schedule publish, this should *not* modify
            // HAS in any way
            hm.publishQueuedHistory();

            // First, ensure bucket references are intact
            auto pqb = HistoryManager::getBucketsReferencedByPublishQueue(
                app->getConfig());
            auto allBuckets = queuedHAS.allBuckets();
            REQUIRE(std::set<std::string>(allBuckets.begin(),
                                          allBuckets.end()) == pqb);

            // Second, ensure `next` is in the exact same state as when it was
            // queued
            for (uint32_t i = 0; i < LiveBucketList::kNumLevels; i++)
            {
                auto const& currentNext = bl.getLevel(i).getNext();
                auto const& queuedNext = queuedHAS.currentBuckets[i].next;

                // Verify state against potential race: HAS was queued with a
                // merge still in-progress, then dequeued and made live
                // (re-starting any merges in-progress) too early, possibly
                // handing off already resolved merges to publish work.
                REQUIRE(currentNext.hasOutputHash() ==
                        queuedNext.hasOutputHash());
                REQUIRE(currentNext.isClear() == queuedNext.isClear());
            }
        });
}

TEST_CASE("persist publish queue", "[history][publish][acceptance]")
{
    Config cfg(getTestConfig(0, Config::TESTDB_BUCKET_DB_PERSISTENT));

    cfg.MANUAL_CLOSE = false;
    cfg.MAX_CONCURRENT_SUBPROCESSES = 0;
    cfg.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
    TmpDirHistoryConfigurator tcfg;
    cfg = tcfg.configure(cfg, true);

    {
        VirtualClock clock;
        Application::pointer app0 = createTestApplication(clock, cfg);
        auto& hm0 = app0->getHistoryManager();
        while (hm0.getPublishQueueCount() < 5)
        {
            clock.crank(true);
        }
        // We should have published nothing and have the first
        // checkpoint still queued.
        REQUIRE(hm0.getPublishSuccessCount() == 0);
        REQUIRE(HistoryManager::getMinLedgerQueuedToPublish(hm0.getConfig()) ==
                7);

        // Trim history after publishing.
        app0->getMaintainer().performMaintenance(50000);
    }

    cfg.MAX_CONCURRENT_SUBPROCESSES = 32;

    {
        VirtualClock clock;
        Application::pointer app1 = Application::create(clock, cfg, 0);
        app1->getHistoryArchiveManager().initializeHistoryArchive(
            tcfg.getArchiveDirName());
        for (size_t i = 0; i < 100; ++i)
            clock.crank(false);
        app1->start();
        auto& hm1 = app1->getHistoryManager();
        while (hm1.getPublishSuccessCount() < 5)
        {
            clock.crank(true);

            // Trim history after publishing whenever possible.
            app1->getMaintainer().performMaintenance(50000);
        }
        // We should have either an empty publish queue or a
        // ledger sometime after the 5th checkpoint
        auto minLedger =
            HistoryManager::getMinLedgerQueuedToPublish(hm1.getConfig());
        LOG_INFO(DEFAULT_LOG, "minLedger {}", minLedger);
        bool okQueue = minLedger == 0 || minLedger >= 35;
        REQUIRE(okQueue);
    }
}

// The idea with this test is that we join a network and somehow get a gap
// in the SCP voting sequence while we're trying to catchup. This will let
// system catchup just before the gap.
TEST_CASE("catchup with a gap", "[history][catchup][acceptance]")
{
    CatchupSimulation catchupSimulation{};
    auto checkpointLedger = catchupSimulation.getLastCheckpointLedger(1);
    catchupSimulation.ensureOnlineCatchupPossible(checkpointLedger, 5);

    // Catch up successfully the first time
    auto app = catchupSimulation.createCatchupApplication(
        std::numeric_limits<uint32_t>::max(), Config::TESTDB_DEFAULT, "app2");
    REQUIRE(catchupSimulation.catchupOnline(app, checkpointLedger, 5));

    // Now generate a little more history
    checkpointLedger = catchupSimulation.getLastCheckpointLedger(2);
    catchupSimulation.ensureOnlineCatchupPossible(checkpointLedger, 5);

    auto init = app->getLedgerManager().getLastClosedLedgerNum() + 2;
    REQUIRE(init == 73);

    // Now start a catchup on that catchups as far as it can due to gap. Make
    // sure gap is past the checkpoint to ensure we buffer the ledger
    LOG_INFO(DEFAULT_LOG, "Starting catchup (with gap) from {}", init);
    REQUIRE(!catchupSimulation.catchupOnline(app, init, 5, init + 59));

    // 73+59=132 is the missing ledger, so the previous ledger was the last one
    // to be closed
    REQUIRE(app->getLedgerManager().getLastClosedLedgerNum() == 131);

    // Now generate a little more history
    checkpointLedger = catchupSimulation.getLastCheckpointLedger(3);
    catchupSimulation.ensureOnlineCatchupPossible(checkpointLedger, 5);

    // And catchup successfully
    CHECK(catchupSimulation.catchupOnline(app, checkpointLedger, 5));
}

/*
 * Test a variety of orderings of CATCHUP_RECENT mode, to shake out boundary
 * cases.
 */
TEST_CASE("Catchup recent", "[history][catchup][acceptance]")
{
    CatchupSimulation catchupSimulation{};
    auto checkpointLedger = catchupSimulation.getLastCheckpointLedger(3);
    catchupSimulation.ensureOnlineCatchupPossible(checkpointLedger, 5);

    auto dbMode = Config::TESTDB_DEFAULT;
    std::vector<Application::pointer> apps;

    // Network has published 0x3f (63), 0x7f (127) and 0xbf (191)
    // Network is currently sitting on ledger 0xc1 (193)

    // Check that isolated catchups work at a variety of boundary
    // conditions relative to the size of a checkpoint:
    std::vector<uint32_t> recents = {0,   1,   2,   31,  32,  33,  62,  63,
                                     64,  65,  66,  126, 127, 128, 129, 130,
                                     190, 191, 192, 193, 194, 1000};

    for (auto r : recents)
    {
        auto name = std::string("catchup-recent-") + std::to_string(r);
        auto app = catchupSimulation.createCatchupApplication(r, dbMode, name);
        REQUIRE(catchupSimulation.catchupOnline(app, checkpointLedger, 5));
        apps.push_back(app);
    }

    // Now push network along a little bit and see that they can all still
    // catch up properly.
    checkpointLedger = catchupSimulation.getLastCheckpointLedger(5);
    catchupSimulation.ensureOnlineCatchupPossible(checkpointLedger, 5);

    for (auto const& app : apps)
    {
        REQUIRE(catchupSimulation.catchupOnline(app, checkpointLedger, 5));
    }

    // Now push network along a _lot_ further along see that they can all
    // still catch up properly.
    checkpointLedger = catchupSimulation.getLastCheckpointLedger(30);
    catchupSimulation.ensureOnlineCatchupPossible(checkpointLedger, 5);

    for (auto const& app : apps)
    {
        REQUIRE(catchupSimulation.catchupOnline(app, checkpointLedger, 5));
    }
}

/*
 * Test a variety of LCL/initLedger/count modes.
 */
TEST_CASE("Catchup manual", "[history][catchup][acceptance]")
{
    CatchupSimulation catchupSimulation{};
    auto checkpointLedger = catchupSimulation.getLastCheckpointLedger(6);
    catchupSimulation.ensureOfflineCatchupPossible(checkpointLedger);
    auto dbMode = Config::TESTDB_DEFAULT;

    // Test every 10th scenario
    for (size_t i = 0; i < stellar::gCatchupRangeCases.size(); i += 10)
    {
        auto test = stellar::gCatchupRangeCases[i];
        auto configuration = test.second;
        auto name =
            fmt::format("lcl = {}, to ledger = {}, count = {}", test.first,
                        configuration.toLedger(), configuration.count());
        LOG_INFO(DEFAULT_LOG, "Catchup configuration: {}", name);
        // manual catchup-recent
        auto app = catchupSimulation.createCatchupApplication(
            configuration.count(), dbMode, name);
        REQUIRE(
            catchupSimulation.catchupOffline(app, configuration.toLedger()));
    }
}

// Check that initializing a history store that already exists, fails.
TEST_CASE("initialize existing history store fails", "[history]")
{
    Config cfg(getTestConfig(0, Config::TESTDB_BUCKET_DB_PERSISTENT));
    TmpDirHistoryConfigurator tcfg;
    cfg = tcfg.configure(cfg, true);

    {
        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg);
        REQUIRE(app->getHistoryArchiveManager().initializeHistoryArchive(
            tcfg.getArchiveDirName()));
    }

    {
        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg);
        REQUIRE(!app->getHistoryArchiveManager().initializeHistoryArchive(
            tcfg.getArchiveDirName()));
    }
}

TEST_CASE("Catchup failure recovery with buffered checkpoint",
          "[history][catchup]")
{
    CatchupSimulation catchupSimulation{};
    auto checkpointLedger = catchupSimulation.getLastCheckpointLedger(1);
    catchupSimulation.ensureOnlineCatchupPossible(checkpointLedger, 5);

    // Catch up successfully the first time
    auto app = catchupSimulation.createCatchupApplication(
        std::numeric_limits<uint32_t>::max(), Config::TESTDB_DEFAULT, "app2");
    REQUIRE(catchupSimulation.catchupOnline(app, checkpointLedger, 5));

    auto init = app->getLedgerManager().getLastClosedLedgerNum() + 2;
    REQUIRE(init == 73);

    // Now generate a little more history
    checkpointLedger = catchupSimulation.getLastCheckpointLedger(2);
    catchupSimulation.ensureOnlineCatchupPossible(checkpointLedger, 63);

    // Now start a catchup on that catchups as far as it can due to gap
    // ledger 133 (= init + 60) will be missing.
    // The app hears up to ledger 189 close. (189 = 129 + 60 where 129
    // is the trigger ledger and 60 is the number of ledgers to buffer.)
    // Then mSyncingLedgers = {128, 129, ..., 132, 134, ..., 189}.
    // We get up to 127 using checkpoint data, and apply {128, ..., 132}.
    LOG_INFO(DEFAULT_LOG, "Starting catchup (with gap) from {}", init);
    REQUIRE(!catchupSimulation.catchupOnline(app, init, 60, init + 60));
    REQUIRE(app->getLedgerManager().getLastClosedLedgerNum() == 132);

    // Now generate a little more history
    checkpointLedger = catchupSimulation.getLastCheckpointLedger(3);
    catchupSimulation.ensureOnlineCatchupPossible(checkpointLedger, 5);

    // 1. LCL is 132
    // 2. LedgerApplyManager has ledgers up to 189.
    //    Once it hears ledger 192 close, it removes ledgers <= 191.
    // 3. Catchup to 191 = initLedger, and then externalize 194 to start catchup
    CHECK(catchupSimulation.catchupOnline(app, checkpointLedger, 1));
}

TEST_CASE("Change ordering of buffered ledgers", "[history][catchup]")
{
    CatchupSimulation catchupSimulation{};

    auto app = catchupSimulation.createCatchupApplication(
        std::numeric_limits<uint32_t>::max(), Config::TESTDB_DEFAULT, "app2");

    auto checkpointLedger = catchupSimulation.getLastCheckpointLedger(1);
    catchupSimulation.ensureOnlineCatchupPossible(checkpointLedger, 15);

    // we have 3 buffered ledgers after trigger (66, 67, and 68)

    SECTION("Checkpoint and trigger in order")
    {
        REQUIRE(catchupSimulation.catchupOnline(app, checkpointLedger, 3, 0, 0,
                                                {63, 64, 65, 67, 68, 66}));
    }

    SECTION("Checkpoint and trigger with gap in between")
    {
        REQUIRE(catchupSimulation.catchupOnline(app, checkpointLedger, 3, 0, 0,
                                                {63, 64, 67, 65, 68, 66}));
    }

    SECTION("Trigger and checkpoint with gap in between")
    {
        REQUIRE(catchupSimulation.catchupOnline(app, checkpointLedger, 3, 0, 0,
                                                {63, 65, 67, 64, 68, 66}));
    }

    SECTION("Reverse order")
    {
        REQUIRE(catchupSimulation.catchupOnline(app, checkpointLedger, 3, 0, 0,
                                                {68, 67, 66, 65, 64, 63}));
    }
}

TEST_CASE("Introduce and fix gap without starting catchup",
          "[history][catchup]")
{
    CatchupSimulation catchupSimulation{};

    auto app = catchupSimulation.createCatchupApplication(
        std::numeric_limits<uint32_t>::max(), Config::TESTDB_DEFAULT, "app2");

    auto checkpointLedger = catchupSimulation.getLastCheckpointLedger(1);
    catchupSimulation.ensureOnlineCatchupPossible(checkpointLedger, 15);
    REQUIRE(catchupSimulation.catchupOnline(app, checkpointLedger, 5));

    auto& lm = app->getLedgerManager();
    auto& lam = app->getLedgerApplyManager();
    auto& herder = static_cast<HerderImpl&>(app->getHerder());

    auto nextLedger = lm.getLastClosedLedgerNum() + 1;

    // introduce 2 gaps (+1 and +4 are missing)
    catchupSimulation.externalizeLedger(herder, nextLedger);
    catchupSimulation.externalizeLedger(herder, nextLedger + 2);
    catchupSimulation.externalizeLedger(herder, nextLedger + 3);
    catchupSimulation.externalizeLedger(herder, nextLedger + 5);
    REQUIRE(!lm.isSynced());
    REQUIRE(lam.getLargestLedgerSeqHeard() > lm.getLastClosedLedgerNum());

    // Fill in the first gap. There will still be buffered ledgers left because
    // of the second gap
    catchupSimulation.externalizeLedger(herder, nextLedger + 1);
    REQUIRE(!lm.isSynced());
    REQUIRE(lam.getLargestLedgerSeqHeard() > lm.getLastClosedLedgerNum());

    // Fill in the second gap. All buffered ledgers should be applied, but we
    // wait for another ledger to close to get in sync
    catchupSimulation.externalizeLedger(herder, nextLedger + 4);
    REQUIRE(lm.isSynced());
    REQUIRE(lam.getLargestLedgerSeqHeard() == lm.getLastClosedLedgerNum());
    REQUIRE(!lam.isCatchupInitialized());
    REQUIRE(lm.getLastClosedLedgerNum() == nextLedger + 5);
}

TEST_CASE("Receive trigger and checkpoint ledger out of order",
          "[history][catchup]")
{
    CatchupSimulation catchupSimulation{};

    auto app = catchupSimulation.createCatchupApplication(
        std::numeric_limits<uint32_t>::max(), Config::TESTDB_DEFAULT, "app2");

    auto& lm = app->getLedgerManager();
    auto& lam = app->getLedgerApplyManager();
    auto& herder = static_cast<HerderImpl&>(app->getHerder());

    auto checkpointLedger = catchupSimulation.getLastCheckpointLedger(1);
    catchupSimulation.ensureOnlineCatchupPossible(checkpointLedger, 60);
    REQUIRE(catchupSimulation.catchupOnline(app, checkpointLedger, 60));

    REQUIRE(app->getLedgerManager().getLastClosedLedgerNum() == 126);

    // Now generate a little more history
    checkpointLedger = catchupSimulation.getLastCheckpointLedger(2);
    catchupSimulation.ensureOnlineCatchupPossible(checkpointLedger, 5);

    // introduce a gap (checkpoint ledger is missing) and then fill it in
    catchupSimulation.externalizeLedger(herder, checkpointLedger + 1);
    catchupSimulation.externalizeLedger(herder, checkpointLedger);
    catchupSimulation.externalizeLedger(herder, checkpointLedger + 2);
    testutil::crankFor(app->getClock(), std::chrono::seconds(10));

    REQUIRE(lm.isSynced());
    REQUIRE(lam.getLargestLedgerSeqHeard() == lm.getLastClosedLedgerNum());
    REQUIRE(!lam.isCatchupInitialized());
    REQUIRE(app->getLedgerManager().getLastClosedLedgerNum() ==
            checkpointLedger + 2);
}

TEST_CASE("Externalize gap while catchup work is running", "[history][catchup]")
{
    CatchupSimulation catchupSimulation{};

    auto app = catchupSimulation.createCatchupApplication(
        std::numeric_limits<uint32_t>::max(), Config::TESTDB_DEFAULT, "app2");

    auto checkpointLedger = catchupSimulation.getLastCheckpointLedger(1);
    catchupSimulation.ensureOnlineCatchupPossible(checkpointLedger, 60);
    REQUIRE(catchupSimulation.catchupOnline(app, checkpointLedger, 60));

    auto lcl = app->getLedgerManager().getLastClosedLedgerNum();
    REQUIRE(lcl == 126);

    // Now generate a little more history
    checkpointLedger = catchupSimulation.getLastCheckpointLedger(2);
    catchupSimulation.ensureOnlineCatchupPossible(checkpointLedger, 10);

    // lcl is 126, so start catchup by skipping 127. Once catchup starts,
    // externalize 127. This ledger will be ignored because catchup is running.
    REQUIRE(catchupSimulation.catchupOnline(app, lcl + 2, 0, 0, 0,
                                            {128, 129, 127}));
}

TEST_CASE("CheckpointBuilder", "[history][publish]")
{
    VirtualClock clock;
    auto cfg = getTestConfig(0, Config::TESTDB_BUCKET_DB_PERSISTENT);
    TmpDirHistoryConfigurator().configure(cfg, true);

    auto app = createTestApplication(clock, cfg);
    releaseAssert(app->getLedgerManager().getLastClosedLedgerNum() ==
                  LedgerManager::GENESIS_LEDGER_SEQ);
    auto& hm = static_cast<HistoryManagerImpl&>(app->getHistoryManager());
    auto& cb = hm.getCheckpointBuilder();
    auto lcl = app->getLedgerManager().getLastClosedLedgerNum();

    auto generate = [&](uint32_t count, bool appendHeaders = true) {
        for (int i = lcl; i < lcl + count; ++i)
        {
            LedgerHeaderHistoryEntry lh;
            lh.header.ledgerSeq = i;
            cb.appendTransactionSet(i, TxSetXDRFrame::makeEmpty(lh),
                                    TransactionResultSet{});
            // Do not append last ledger in a checkpoint if `appendHeaders` is
            // false
            if (!appendHeaders && i == count)
            {
                continue;
            }
            cb.appendLedgerHeader(lh.header);
        }
    };

    SECTION("recover")
    {
        SECTION("recover transactions, but not headers")
        {
            generate(10, false);
            validateCheckpointFiles(*app, 9, false);
        }
        SECTION("recover both")
        {
            generate(10);
            validateCheckpointFiles(*app, 10, false);
        }
        SECTION("recover due to partial write")
        {
            generate(10);
            validateCheckpointFiles(*app, 10, false);
            FileTransferInfo headers(FileType::HISTORY_FILE_TYPE_LEDGER,
                                     HistoryManager::checkpointContainingLedger(
                                         10, app->getConfig()),
                                     app->getConfig());
            auto sz =
                std::filesystem::file_size(headers.localPath_nogz_dirty());
            std::filesystem::resize_file(headers.localPath_nogz_dirty(),
                                         sz - 1);
        }
        CheckpointBuilder cb2{*app};
        cb2.cleanup(9);
        validateCheckpointFiles(*app, 9, false);
    }
    SECTION("checkpoint complete")
    {
        auto ledgerSeq = HistoryManager::checkpointContainingLedger(1, cfg);
        // Checkpoint not finalized
        generate(ledgerSeq);
        validateCheckpointFiles(*app, ledgerSeq, false);
        cb.checkpointComplete(ledgerSeq);
        validateCheckpointFiles(*app, ledgerSeq, true);
        REQUIRE(!cb.mOpen);
        // any subssequent call to checkpointComplete is a no-op
        cb.checkpointComplete(ledgerSeq);
        validateCheckpointFiles(*app, ledgerSeq, true);
    }
}
