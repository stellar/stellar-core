// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "history/test/HistoryTestsUtils.h"
#include "bucket/BucketManager.h"
#include "catchup/CatchupRange.h"
#include "crypto/Hex.h"
#include "crypto/Random.h"
#include "herder/TxSetFrame.h"
#include "history/FileTransferInfo.h"
#include "history/HistoryArchiveManager.h"
#include "ledger/CheckpointRange.h"
#include "ledger/LedgerRange.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnHeader.h"
#include "lib/catch.hpp"
#include "main/ApplicationUtils.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "util/Math.h"
#include "util/XDROperators.h"
#include "work/WorkScheduler.h"

using namespace stellar;
using namespace txtest;

namespace stellar
{
namespace historytestutils
{

std::string
HistoryConfigurator::getArchiveDirName() const
{
    return "";
}

TmpDirHistoryConfigurator::TmpDirHistoryConfigurator()
    : mName("archtmp-" + binToHex(randomBytes(8))), mArchtmp(mName)
{
}

std::string
TmpDirHistoryConfigurator::getArchiveDirName() const
{
    return mName;
}

Config&
TmpDirHistoryConfigurator::configure(Config& cfg, bool writable) const
{
    std::string d = getArchiveDirName();
    std::string getCmd = "cp " + d + "/{0} {1}";
    std::string putCmd = "";
    std::string mkdirCmd = "";

    if (writable)
    {
        putCmd = "cp {0} " + d + "/{1}";
        mkdirCmd = "mkdir -p " + d + "/{0}";
    }

    cfg.HISTORY[d] = HistoryArchiveConfiguration{d, getCmd, putCmd, mkdirCmd};
    cfg.TESTING_SOROBAN_HIGH_LIMIT_OVERRIDE = true;
    return cfg;
}

MultiArchiveHistoryConfigurator::MultiArchiveHistoryConfigurator(
    uint32_t numArchives)
{
    while (numArchives > 0)
    {
        auto conf = std::make_shared<TmpDirHistoryConfigurator>();
        mConfigurators.emplace_back(conf);
        --numArchives;
    }
}

Config&
MultiArchiveHistoryConfigurator::configure(Config& cfg, bool writable) const
{
    for (auto const& conf : mConfigurators)
    {
        conf->configure(cfg, writable);
    }
    REQUIRE(cfg.HISTORY.size() == mConfigurators.size());
    return cfg;
}

Config&
S3HistoryConfigurator::configure(Config& mCfg, bool writable) const
{
    char const* s3bucket = getenv("S3BUCKET");
    if (!s3bucket)
    {
        throw std::runtime_error("s3 test requires S3BUCKET env var");
    }
    std::string s3b(s3bucket);
    if (s3b.find("s3://") != 0)
    {
        s3b = std::string("s3://") + s3b;
    }
    std::string getCmd = "aws s3 cp " + s3b + "/{0} {1}";
    std::string putCmd = "";
    std::string mkdirCmd = "";
    if (writable)
    {
        putCmd = "aws s3 cp {0} " + s3b + "/{1}";
    }
    mCfg.HISTORY["test"] =
        HistoryArchiveConfiguration{"test", getCmd, putCmd, mkdirCmd};
    return mCfg;
}

Config&
RealGenesisTmpDirHistoryConfigurator::configure(Config& mCfg,
                                                bool writable) const
{
    TmpDirHistoryConfigurator::configure(mCfg, writable);
    mCfg.USE_CONFIG_FOR_GENESIS = false;
    return mCfg;
}

BucketOutputIteratorForTesting::BucketOutputIteratorForTesting(
    std::string const& tmpDir, uint32_t protocolVersion, MergeCounters& mc,
    asio::io_context& ctx)
    : BucketOutputIterator{
          tmpDir, true, testutil::testBucketMetadata(protocolVersion),
          mc,     ctx,  /*doFsync=*/true}
{
}

std::pair<std::string, uint256>
BucketOutputIteratorForTesting::writeTmpTestBucket()
{
    auto ledgerEntries =
        LedgerTestUtils::generateValidUniqueLedgerEntries(NUM_ITEMS_PER_BUCKET);
    auto bucketEntries =
        LiveBucket::convertToBucketEntry(false, {}, ledgerEntries, {});
    for (auto const& bucketEntry : bucketEntries)
    {
        put(bucketEntry);
    }

    // Finish writing and close the bucket file
    REQUIRE(mBuf);
    mOut.writeOne(*mBuf, &mHasher, &mBytesPut);
    mObjectsPut++;
    mBuf.reset();
    mOut.close();

    return std::pair<std::string, uint256>(mFilename.string(),
                                           mHasher.finish());
};

TestBucketGenerator::TestBucketGenerator(
    Application& app, std::shared_ptr<HistoryArchive> archive)
    : mApp{app}, mArchive{archive}
{
    mTmpDir = std::make_unique<TmpDir>(
        mApp.getTmpDirManager().tmpDir("tmp-bucket-generator"));
}

std::string
TestBucketGenerator::generateBucket(TestBucketState state)
{
    uint256 hash = HashUtils::pseudoRandomForTesting();
    if (state == TestBucketState::FILE_NOT_UPLOADED)
    {
        // Skip uploading the file, return any hash
        return binToHex(hash);
    }
    MergeCounters mc;
    BucketOutputIteratorForTesting bucketOut{
        mTmpDir->getName(), mApp.getConfig().LEDGER_PROTOCOL_VERSION, mc,
        mApp.getClock().getIOContext()};
    std::string filename;
    std::tie(filename, hash) = bucketOut.writeTmpTestBucket();

    if (state == TestBucketState::HASH_MISMATCH)
    {
        hash = HashUtils::pseudoRandomForTesting();
    }

    // Upload generated bucket to the archive
    {
        FileTransferInfo ft{mTmpDir->getName(),
                            FileType::HISTORY_FILE_TYPE_BUCKET, binToHex(hash)};
        auto& wm = mApp.getWorkScheduler();
        auto put = std::make_shared<PutRemoteFileWork>(
            mApp, filename + ".gz", ft.remoteName(), mArchive);
        auto mkdir =
            std::make_shared<MakeRemoteDirWork>(mApp, ft.remoteDir(), mArchive);

        std::vector<std::shared_ptr<BasicWork>> seq;

        if (state != TestBucketState::CORRUPTED_ZIPPED_FILE)
        {
            seq = {std::make_shared<GzipFileWork>(mApp, filename, true), mkdir,
                   put};
        }
        else
        {
            std::ofstream out;
            out.exceptions(std::ios::failbit | std::ios::badbit);
            out.open(filename + ".gz");
            out.close();
            seq = {mkdir, put};
        }

        wm.scheduleWork<WorkSequence>("bucket-publish-seq", seq);
        while (!mApp.getClock().getIOContext().stopped() &&
               !wm.allChildrenDone())
        {
            mApp.getClock().crank(true);
        }
    }

    return binToHex(hash);
}

TestLedgerChainGenerator::TestLedgerChainGenerator(
    Application& app, std::shared_ptr<HistoryArchive> archive,
    CheckpointRange range, TmpDir const& tmpDir)
    : mApp{app}, mArchive{archive}, mCheckpointRange{range}, mTmpDir{tmpDir}
{
}

void
TestLedgerChainGenerator::createHistoryFiles(
    std::vector<LedgerHeaderHistoryEntry> const& lhv,
    LedgerHeaderHistoryEntry& first, LedgerHeaderHistoryEntry& last,
    uint32_t checkpoint)
{
    FileTransferInfo ft{mTmpDir, FileType::HISTORY_FILE_TYPE_LEDGER,
                        checkpoint};
    XDROutputFileStream ledgerOut(mApp.getClock().getIOContext(),
                                  /*doFsync=*/true);
    ledgerOut.open(ft.localPath_nogz());

    for (auto& ledger : lhv)
    {
        if (first.header.ledgerSeq == 0)
        {
            first = ledger;
        }
        REQUIRE_NOTHROW(ledgerOut.writeOne(ledger));
        last = ledger;
    }
    ledgerOut.close();
}

TestLedgerChainGenerator::CheckpointEnds
TestLedgerChainGenerator::makeOneLedgerFile(
    uint32_t currCheckpoint, Hash prevHash,
    HistoryManager::LedgerVerificationStatus state)
{
    auto initLedger = HistoryManager::firstLedgerInCheckpointContaining(
        currCheckpoint, mApp.getConfig());
    auto size = HistoryManager::sizeOfCheckpointContaining(currCheckpoint,
                                                           mApp.getConfig());

    LedgerHeaderHistoryEntry first, last, lcl;
    lcl.header.ledgerSeq = initLedger;
    lcl.header.previousLedgerHash = prevHash;

    std::vector<LedgerHeaderHistoryEntry> ledgerChain =
        LedgerTestUtils::generateLedgerHeadersForCheckpoint(lcl, size, state);

    createHistoryFiles(ledgerChain, first, last, currCheckpoint);
    return CheckpointEnds(first, last);
}

TestLedgerChainGenerator::CheckpointEnds
TestLedgerChainGenerator::makeLedgerChainFiles(
    HistoryManager::LedgerVerificationStatus state)
{
    Hash hash = HashUtils::pseudoRandomForTesting();
    LedgerHeaderHistoryEntry beginRange;

    LedgerHeaderHistoryEntry first, last;
    for (auto i = mCheckpointRange.mFirst; i < mCheckpointRange.limit();
         i += HistoryManager::sizeOfCheckpointContaining(i, mApp.getConfig()))
    {
        // Only corrupt first checkpoint (last to be verified)
        if (i != mCheckpointRange.mFirst)
        {
            state = HistoryManager::VERIFY_STATUS_OK;
        }

        std::tie(first, last) = makeOneLedgerFile(i, hash, state);
        hash = last.hash;

        if (beginRange.header.ledgerSeq == 0)
        {
            beginRange = first;
        }
    }

    return CheckpointEnds(beginRange, last);
}

CatchupPerformedWork::CatchupPerformedWork(
    LedgerApplyManager::CatchupMetrics const& metrics)
    : mHistoryArchiveStatesDownloaded{metrics.mHistoryArchiveStatesDownloaded}
    , mCheckpointsDownloaded{metrics.mCheckpointsDownloaded}
    , mLedgersVerified{metrics.mLedgersVerified}
    , mLedgerChainsVerificationFailed{metrics.mLedgerChainsVerificationFailed}
    , mBucketsDownloaded{metrics.mBucketsDownloaded > 0}
    , mBucketsApplied{metrics.mBucketsApplied > 0}
    , mTxSetsDownloaded{metrics.mTxSetsDownloaded}
    , mTxSetsApplied{metrics.mTxSetsApplied}
{
}

CatchupPerformedWork::CatchupPerformedWork(
    uint64_t historyArchiveStatesDownloaded, uint64_t checkpointsDownloaded,
    uint64_t ledgersVerified, uint64_t ledgerChainsVerificationFailed,
    bool bucketsDownloaded, bool bucketsApplied, uint64_t txSetsDownloaded,
    uint64_t txSetsApplied)
    : mHistoryArchiveStatesDownloaded{historyArchiveStatesDownloaded}
    , mCheckpointsDownloaded{checkpointsDownloaded}
    , mLedgersVerified{ledgersVerified}
    , mLedgerChainsVerificationFailed{ledgerChainsVerificationFailed}
    , mBucketsDownloaded{bucketsDownloaded}
    , mBucketsApplied{bucketsApplied}
    , mTxSetsDownloaded{txSetsDownloaded}
    , mTxSetsApplied{txSetsApplied}
{
}

bool
operator==(CatchupPerformedWork const& x, CatchupPerformedWork const& y)
{
    if (x.mHistoryArchiveStatesDownloaded != y.mHistoryArchiveStatesDownloaded)
    {
        return false;
    }
    if (x.mCheckpointsDownloaded != y.mCheckpointsDownloaded)
    {
        return false;
    }
    if (x.mLedgersVerified != y.mLedgersVerified)
    {
        return false;
    }
    if (x.mLedgerChainsVerificationFailed != y.mLedgerChainsVerificationFailed)
    {
        return false;
    }
    if (x.mBucketsDownloaded != y.mBucketsDownloaded)
    {
        return false;
    }
    if (x.mBucketsApplied != y.mBucketsApplied)
    {
        return false;
    }
    if (x.mTxSetsDownloaded != y.mTxSetsDownloaded)
    {
        return false;
    }
    if (x.mTxSetsApplied != y.mTxSetsApplied)
    {
        return false;
    }
    return true;
}

bool
operator!=(CatchupPerformedWork const& x, CatchupPerformedWork const& y)
{
    return !(x == y);
}

CatchupSimulation::CatchupSimulation(VirtualClock::Mode mode,
                                     std::shared_ptr<HistoryConfigurator> cg,
                                     bool startApp, Config::TestDbMode dbMode)
    : mClock(std::make_unique<VirtualClock>(mode))
    , mHistoryConfigurator(cg)
    , mCfg(getTestConfig(0, dbMode))
    , mAppPtr(createTestApplication(*mClock,
                                    mHistoryConfigurator->configure(mCfg, true),
                                    /*newDB*/ true, /*startApp*/ false))
{
    auto dirName = cg->getArchiveDirName();
    if (!dirName.empty())
    {
        CHECK(getApp().getHistoryArchiveManager().initializeHistoryArchive(
            dirName));
    }
    if (startApp)
    {
        mAppPtr->start();
    }
}

CatchupSimulation::~CatchupSimulation()
{
}

uint32_t
CatchupSimulation::getLastCheckpointLedger(uint32_t checkpointIndex) const
{
    return HistoryManager::getCheckpointFrequency(getApp().getConfig()) *
               checkpointIndex -
           1;
}

void
CatchupSimulation::generateRandomLedger(uint32_t version)
{
    auto& lm = getApp().getLedgerManager();
    uint32_t ledgerSeq = lm.getLastClosedLedgerNum() + 1;
    uint64_t minBalance = lm.getLastMinBalance(5);
    uint64_t big = minBalance + ledgerSeq;
    uint64_t small = 100 + ledgerSeq;
    uint64_t closeTime = 60 * 5 * ledgerSeq;

    auto root = TestAccount{getApp(), getRoot(getApp().getNetworkID())};
    auto alice = TestAccount{getApp(), getAccount("alice")};
    auto bob = TestAccount{getApp(), getAccount("bob")};
    auto carol = TestAccount{getApp(), getAccount("carol")};
    auto eve = TestAccount{getApp(), getAccount("eve")};
    auto stroopy = TestAccount{getApp(), getAccount("stroopy")};

    std::vector<TransactionFrameBasePtr> txs;
    std::vector<TransactionFrameBasePtr> sorobanTxs;
    bool check = false;

    if (ledgerSeq < 5)
    {
        txs.push_back(root.tx(
            {createAccount(alice, big), createAccount(bob, big),
             createAccount(carol, big), createAccount(stroopy, big * 10),
             createAccount(eve, big * 10)}));
    }
    // Allow an occasional empty ledger (but always have some transactions in
    // the upgrade ledger)
    else if ((rand_flip() || rand_flip()) ||
             lm.getLastClosedLedgerNum() + 1 == mUpgradeLedgerSeq)
    {
        // They all randomly send a little to one another every ledger after #4
        if (rand_flip())
        {
            txs.push_back(root.tx({payment(alice, big)}));
        }
        else
        {
            txs.push_back(root.tx({payment(bob, big)}));
        }

        if (rand_flip())
        {
            txs.push_back(alice.tx({payment(bob, small)}));
        }
        else
        {
            txs.push_back(alice.tx({payment(carol, small)}));
        }

        if (rand_flip())
        {
            txs.push_back(bob.tx({payment(alice, small)}));
        }
        else
        {
            txs.push_back(bob.tx({payment(carol, small)}));
        }

        if (rand_flip())
        {
            txs.push_back(carol.tx({payment(alice, small)}));
        }
        else
        {
            txs.push_back(carol.tx({payment(bob, small)}));
        }

        // Add soroban transactions
        if (protocolVersionStartsFrom(
                lm.getLastClosedLedgerHeader().header.ledgerVersion,
                SOROBAN_PROTOCOL_VERSION))
        {
            SorobanResources res;
            res.instructions = getApp()
                                   .getLedgerManager()
                                   .maxSorobanTransactionResources()
                                   .getVal(Resource::Type::INSTRUCTIONS) /
                               10;
            res.writeBytes = 100'000;
            uint32_t inclusion = 100;
            sorobanTxs.push_back(createUploadWasmTx(
                getApp(), stroopy, inclusion, DEFAULT_TEST_RESOURCE_FEE, res));
            sorobanTxs.push_back(createUploadWasmTx(
                getApp(), eve, inclusion * 5, DEFAULT_TEST_RESOURCE_FEE, res));
            check = true;
        }
    }

    auto phases = protocolVersionStartsFrom(
                      lm.getLastClosedLedgerHeader().header.ledgerVersion,
                      SOROBAN_PROTOCOL_VERSION)
                      ? PerPhaseTransactionList{txs, sorobanTxs}
                      : PerPhaseTransactionList{txs};
    TxSetXDRFrameConstPtr txSet =
        makeTxSetFromTransactions(phases, getApp(), 0, 0).first;

    CLOG_INFO(History, "Closing synthetic ledger {} with {} txs (txhash:{})",
              ledgerSeq, txSet->sizeTxTotal(),
              hexAbbrev(txSet->getContentsHash()));

    auto upgrades = xdr::xvector<UpgradeType, 6>{};
    if (lm.getLastClosedLedgerHeader().header.ledgerVersion < version)
    {
        auto ledgerUpgrade = LedgerUpgrade{LEDGER_UPGRADE_VERSION};
        ledgerUpgrade.newLedgerVersion() = version;
        auto v = xdr::xdr_to_opaque(ledgerUpgrade);
        upgrades.push_back(UpgradeType{v.begin(), v.end()});
    }

    StellarValue sv = getApp().getHerder().makeStellarValue(
        txSet->getContentsHash(), closeTime, upgrades,
        getApp().getConfig().NODE_SEED);

    mLedgerCloseDatas.emplace_back(ledgerSeq, txSet, sv);

    auto& txsSucceeded =
        getApp().getMetrics().NewCounter({"ledger", "apply", "success"});
    auto lastSucceeded = txsSucceeded.count();

    lm.closeLedger(mLedgerCloseDatas.back());

    if (check)
    {
        // Make sure all classic transactions and at least some Soroban
        // transactions succeeded
        REQUIRE(txsSucceeded.count() > lastSucceeded + phases[0].size());
    }

    auto const& lclh = lm.getLastClosedLedgerHeader();
    mLedgerSeqs.push_back(lclh.header.ledgerSeq);
    mLedgerHashes.push_back(lclh.hash);
    mBucketListHashes.push_back(lclh.header.bucketListHash);
    mBucket0Hashes.push_back(getApp()
                                 .getBucketManager()
                                 .getLiveBucketList()
                                 .getLevel(0)
                                 .getCurr()
                                 ->getHash());
    mBucket1Hashes.push_back(getApp()
                                 .getBucketManager()
                                 .getLiveBucketList()
                                 .getLevel(2)
                                 .getCurr()
                                 ->getHash());

    rootBalances.push_back(root.getBalance());
    aliceBalances.push_back(alice.getBalance());
    bobBalances.push_back(bob.getBalance());
    carolBalances.push_back(carol.getBalance());
    eveBalances.push_back(eve.getBalance());
    stroopyBalances.push_back(stroopy.getBalance());

    rootSeqs.push_back(root.loadSequenceNumber());
    aliceSeqs.push_back(alice.loadSequenceNumber());
    bobSeqs.push_back(bob.loadSequenceNumber());
    carolSeqs.push_back(carol.loadSequenceNumber());
    eveSeqs.push_back(eve.loadSequenceNumber());
    stroopySeqs.push_back(stroopy.loadSequenceNumber());
}

void
CatchupSimulation::setUpgradeLedger(uint32_t ledger,
                                    ProtocolVersion upgradeProtocolVersion)
{
    REQUIRE(getApp().getLedgerManager().getLastClosedLedgerNum() < ledger);
    mUpgradeLedgerSeq = ledger;
    mUpgradeProtocolVersion = upgradeProtocolVersion;
}

void
CatchupSimulation::ensureLedgerAvailable(uint32_t targetLedger,
                                         std::optional<uint32_t> restartLedger)
{
    while (getApp().getLedgerManager().getLastClosedLedgerNum() < targetLedger)
    {
        if (restartLedger &&
            getApp().getLedgerManager().getLastClosedLedgerNum() ==
                *restartLedger)
        {
            REQUIRE(*restartLedger < targetLedger);
            restartApp();
        }
        auto lcl = getApp().getLedgerManager().getLastClosedLedgerNum();
        if (lcl + 1 == mUpgradeLedgerSeq)
        {
            // Force protocol upgrade
            generateRandomLedger(
                static_cast<uint32_t>(mUpgradeProtocolVersion));
        }
        else
        {
            generateRandomLedger(getApp()
                                     .getLedgerManager()
                                     .getLastClosedLedgerHeader()
                                     .header.ledgerVersion);
        }

        if (HistoryManager::publishCheckpointOnLedgerClose(
                lcl, getApp().getConfig()))
        {
            mBucketListAtLastPublish =
                getApp().getBucketManager().getLiveBucketList();
        }
    }
}

void
CatchupSimulation::ensurePublishesComplete()
{
    auto& hm = getApp().getHistoryManager();
    auto const& cfg = getApp().getConfig();
    while (HistoryManager::publishQueueLength(cfg) > 0 &&
           hm.getPublishFailureCount() == 0)
    {
        getApp().getClock().crank(true);
    }

    REQUIRE(hm.getPublishFailureCount() == 0);
    // Make sure all references to buckets were released
    REQUIRE(HistoryManager::getBucketsReferencedByPublishQueue(cfg).empty());

    // Make sure all published checkpoint files have been cleaned up
    auto lcl = getApp().getLedgerManager().getLastClosedLedgerNum();
    auto firstCheckpoint = HistoryManager::checkpointContainingLedger(
        LedgerManager::GENESIS_LEDGER_SEQ, cfg);
    auto lastCheckpoint =
        HistoryManager::lastLedgerBeforeCheckpointContaining(lcl, cfg);

    for (uint32_t i = firstCheckpoint; i <= lastCheckpoint;
         i += HistoryManager::getCheckpointFrequency(cfg))
    {
        FileTransferInfo res(FileType::HISTORY_FILE_TYPE_RESULTS, i,
                             getApp().getConfig());
        FileTransferInfo txs(FileType::HISTORY_FILE_TYPE_TRANSACTIONS, i,
                             getApp().getConfig());
        FileTransferInfo headers(FileType::HISTORY_FILE_TYPE_LEDGER, i,
                                 getApp().getConfig());
        REQUIRE(!fs::exists(res.localPath_nogz_dirty()));
        REQUIRE(!fs::exists(txs.localPath_nogz_dirty()));
        REQUIRE(!fs::exists(headers.localPath_nogz_dirty()));
        REQUIRE(!fs::exists(res.localPath_nogz()));
        REQUIRE(!fs::exists(txs.localPath_nogz()));
        REQUIRE(!fs::exists(headers.localPath_nogz()));
    }
}

void
CatchupSimulation::ensureOfflineCatchupPossible(
    uint32_t targetLedger, std::optional<uint32_t> restartLedger)
{
    // One additional ledger is needed for publish.
    auto target = HistoryManager::checkpointContainingLedger(
                      targetLedger, getApp().getConfig()) +
                  1;
    ensureLedgerAvailable(target, restartLedger);
    ensurePublishesComplete();
}

void
CatchupSimulation::ensureOnlineCatchupPossible(uint32_t targetLedger,
                                               uint32_t bufferLedgers)
{
    // One additional ledger is needed for publish, one as a trigger ledger for
    // catchup, one as closing ledger.
    ensureLedgerAvailable(HistoryManager::checkpointContainingLedger(
                              targetLedger, getApp().getConfig()) +
                          bufferLedgers + 3);
    ensurePublishesComplete();
}

std::vector<LedgerNumHashPair>
CatchupSimulation::getAllPublishedCheckpoints() const
{
    std::vector<LedgerNumHashPair> res;
    assert(mLedgerHashes.size() == mLedgerSeqs.size());
    auto hi = mLedgerHashes.begin();
    auto si = mLedgerSeqs.begin();
    while (si != mLedgerSeqs.end())
    {
        if (HistoryManager::isLastLedgerInCheckpoint(*si, getApp().getConfig()))
        {
            LedgerNumHashPair pair;
            pair.first = *si;
            pair.second = std::make_optional<Hash>(*hi);
            res.emplace_back(pair);
        }
        ++hi;
        ++si;
    }
    return res;
}

LedgerNumHashPair
CatchupSimulation::getLastPublishedCheckpoint() const
{
    LedgerNumHashPair pair;
    assert(mLedgerHashes.size() == mLedgerSeqs.size());
    auto hi = mLedgerHashes.rbegin();
    auto si = mLedgerSeqs.rbegin();
    while (si != mLedgerSeqs.rend())
    {
        if (HistoryManager::isLastLedgerInCheckpoint(*si, getApp().getConfig()))
        {
            pair.first = *si;
            pair.second = std::make_optional<Hash>(*hi);
            break;
        }
        ++hi;
        ++si;
    }
    return pair;
}

Application::pointer
CatchupSimulation::createCatchupApplication(
    uint32_t count, Config::TestDbMode dbMode, std::string const& appName,
    bool publish, std::optional<uint32_t> ledgerVersion)
{
    CLOG_INFO(History, "****");
    CLOG_INFO(History, "**** Create app for catchup: '{}'", appName);
    CLOG_INFO(History, "****");

    mCfgs.emplace_back(
        getTestConfig(static_cast<int>(mCfgs.size()) + 1, dbMode));
    mCfgs.back().CATCHUP_COMPLETE =
        count == std::numeric_limits<uint32_t>::max();
    mCfgs.back().CATCHUP_RECENT = count;
    if (ledgerVersion)
    {
        mCfgs.back().TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION = *ledgerVersion;
    }
    mSpawnedAppsClocks.emplace_front();
    auto newApp = createTestApplication(
        mSpawnedAppsClocks.front(),
        mHistoryConfigurator->configure(mCfgs.back(), publish));
    newApp->start();
    return newApp;
}

bool
CatchupSimulation::catchupOffline(Application::pointer app, uint32_t toLedger,
                                  bool extraValidation)
{
    CLOG_INFO(History, "starting offline catchup with toLedger={}", toLedger);

    auto startCatchupMetrics = app->getLedgerApplyManager().getCatchupMetrics();
    auto& lm = app->getLedgerManager();
    auto lastLedger = lm.getLastClosedLedgerNum();
    auto mode = extraValidation ? CatchupConfiguration::Mode::OFFLINE_COMPLETE
                                : CatchupConfiguration::Mode::OFFLINE_BASIC;
    auto catchupConfiguration =
        CatchupConfiguration{toLedger, app->getConfig().CATCHUP_RECENT, mode};
    lm.startCatchup(catchupConfiguration, nullptr, {});
    REQUIRE(!app->getClock().getIOContext().stopped());

    auto& lam = app->getLedgerApplyManager();
    auto finished = [&]() { return lam.catchupWorkIsDone(); };

    auto expectedCatchupWork =
        computeCatchupPerformedWork(lastLedger, catchupConfiguration, *app);
    testutil::crankUntil(app, finished,
                         std::chrono::seconds{std::max<int64>(
                             expectedCatchupWork.mTxSetsApplied + 15, 60)});

    // Finished successfully
    auto success = lam.isCatchupInitialized() &&
                   lam.getCatchupWorkState() == BasicWork::State::WORK_SUCCESS;
    if (success)
    {
        CLOG_INFO(History, "Caught up");

        auto endCatchupMetrics =
            app->getLedgerApplyManager().getCatchupMetrics();
        auto catchupPerformedWork =
            CatchupPerformedWork{endCatchupMetrics - startCatchupMetrics};

        REQUIRE(catchupPerformedWork == expectedCatchupWork);
        if (app->getHistoryArchiveManager().publishEnabled())
        {
            auto& hm = app->getHistoryManager();
            REQUIRE(hm.getPublishQueueCount() - hm.getPublishSuccessCount() <=
                    CatchupWork::PUBLISH_QUEUE_MAX_SIZE);
        }
    }

    validateCatchup(app);
    return success;
}

bool
CatchupSimulation::catchupOnline(Application::pointer app, uint32_t initLedger,
                                 uint32_t bufferLedgers, uint32_t gapLedger,
                                 int32_t numGapLedgers,
                                 std::vector<uint32_t> const& ledgersToInject)
{
    auto& lm = app->getLedgerManager();
    auto startCatchupMetrics = app->getLedgerApplyManager().getCatchupMetrics();

    auto& herder = static_cast<HerderImpl&>(app->getHerder());

    // catchup will run to the final ledger in the checkpoint
    auto toLedger = HistoryManager::checkpointContainingLedger(
        initLedger - 1, app->getConfig());

    auto catchupConfiguration =
        CatchupConfiguration{toLedger, app->getConfig().CATCHUP_RECENT,
                             CatchupConfiguration::Mode::ONLINE};

    auto caughtUp = [&]() { return lm.isSynced(); };

    auto externalize = [&](uint32 n) {
        if (numGapLedgers > 0 && n == gapLedger)
        {
            if (--numGapLedgers > 0)
            {
                // skip next ledger as well
                ++gapLedger;
            }

            CLOG_INFO(History,
                      "simulating LedgerClose transmit gap at ledger {}", n);
        }
        else
        {
            externalizeLedger(herder, n);
        }
    };

    // Externalize (to the catchup LM) the range of ledgers between initLedger
    // and as near as we can get to the first ledger of the block after
    // initLedger (inclusive), so that there's something to knit-up with. Do not
    // externalize anything we haven't yet published, of course.
    uint32_t firstLedgerInCheckpoint;
    if (HistoryManager::isFirstLedgerInCheckpoint(initLedger, app->getConfig()))
    {
        firstLedgerInCheckpoint = initLedger;
    }
    else
    {
        firstLedgerInCheckpoint =
            HistoryManager::firstLedgerAfterCheckpointContaining(
                initLedger, app->getConfig());
    }

    uint32_t triggerLedger = HistoryManager::ledgerToTriggerCatchup(
        firstLedgerInCheckpoint, app->getConfig());

    if (ledgersToInject.empty())
    {
        for (uint32_t n = initLedger; n <= triggerLedger + bufferLedgers; ++n)
        {
            externalize(n);
        }
    }
    else
    {
        for (auto ledger : ledgersToInject)
        {
            externalize(ledger);
        }
    }

    if (caughtUp())
    {
        // If at this moment status is LM_SYNCED_STATE, it means that catchup
        // has not started.
        return false;
    }

    auto catchupIsDone = [&]() {
        return app->getLedgerApplyManager().catchupWorkIsDone();
    };

    auto lastLedger = lm.getLastClosedLedgerNum();

    auto expectedCatchupWork =
        computeCatchupPerformedWork(lastLedger, catchupConfiguration, *app);

    testutil::crankUntil(app, catchupIsDone,
                         std::chrono::seconds{std::max<int64>(
                             expectedCatchupWork.mTxSetsApplied + 15, 60)});

    if (lm.getLastClosedLedgerNum() == triggerLedger + bufferLedgers)
    {
        // Externalize closing ledger
        externalize(triggerLedger + bufferLedgers + 1);
    }

    testutil::crankUntil(
        app,
        [&]() {
            return lm.getLastClosedLedgerNum() ==
                   triggerLedger + bufferLedgers + 1;
        },
        std::chrono::seconds{60});

    auto result = caughtUp();
    if (result)
    {
        REQUIRE(lm.getLastClosedLedgerNum() >= triggerLedger + bufferLedgers);

        auto endCatchupMetrics =
            app->getLedgerApplyManager().getCatchupMetrics();
        auto catchupPerformedWork =
            CatchupPerformedWork{endCatchupMetrics - startCatchupMetrics};

        REQUIRE(catchupPerformedWork == expectedCatchupWork);

        CLOG_INFO(History, "Caught up");
    }

    validateCatchup(app);
    return result;
}

void
CatchupSimulation::externalizeLedger(HerderImpl& herder, uint32_t ledger)
{
    // Remember the vectors count from 2, not 0.
    if (ledger - 2 >= mLedgerCloseDatas.size())
    {
        return;
    }

    auto const& lcd = mLedgerCloseDatas.at(ledger - 2);

    CLOG_INFO(History,
              "force-externalizing LedgerCloseData for {} has txhash:{}",
              ledger, hexAbbrev(lcd.getTxSet()->getContentsHash()));

    herder.getPendingEnvelopes().putTxSet(lcd.getTxSet()->getContentsHash(),
                                          lcd.getLedgerSeq(), lcd.getTxSet());
    herder.getHerderSCPDriver().valueExternalized(
        lcd.getLedgerSeq(), xdr::xdr_to_opaque(lcd.getValue()));
}

void
CatchupSimulation::validateCatchup(Application::pointer app)
{
    auto& lm = app->getLedgerManager();
    auto nextLedger = lm.getLastClosedLedgerNum() + 1;

    if (nextLedger < 3)
    {
        return;
    }

    size_t i = nextLedger - 3;

    auto root = TestAccount{*app, getRoot(getApp().getNetworkID())};
    auto alice = TestAccount{*app, getAccount("alice")};
    auto bob = TestAccount{*app, getAccount("bob")};
    auto carol = TestAccount{*app, getAccount("carol")};
    auto eve = TestAccount{*app, getAccount("eve")};
    auto stroopy = TestAccount{*app, getAccount("stroopy")};

    auto wantSeq = mLedgerSeqs.at(i);
    auto wantHash = mLedgerHashes.at(i);
    auto wantBucketListHash = mBucketListHashes.at(i);
    auto wantBucket0Hash = mBucket0Hashes.at(i);
    auto wantBucket1Hash = mBucket1Hashes.at(i);

    auto haveSeq = lm.getLastClosedLedgerNum();
    auto haveHash = lm.getLastClosedLedgerHeader().hash;
    auto haveBucketListHash =
        lm.getLastClosedLedgerHeader().header.bucketListHash;
    auto haveBucket0Hash = app->getBucketManager()
                               .getLiveBucketList()
                               .getLevel(0)
                               .getCurr()
                               ->getHash();
    auto haveBucket1Hash = app->getBucketManager()
                               .getLiveBucketList()
                               .getLevel(2)
                               .getCurr()
                               ->getHash();

    CLOG_INFO(History, "Caught up: want Seq[{}] = {}", i, wantSeq);
    CLOG_INFO(History, "Caught up: have Seq[{}] = {}", i, haveSeq);

    CLOG_INFO(History, "Caught up: want Hash[{}] = {}", i, hexAbbrev(wantHash));
    CLOG_INFO(History, "Caught up: have Hash[{}] = {}", i, hexAbbrev(haveHash));

    CLOG_INFO(History, "Caught up: want BucketListHash[{}] = {}", i,
              hexAbbrev(wantBucketListHash));
    CLOG_INFO(History, "Caught up: have BucketListHash[{}] = {}", i,
              hexAbbrev(haveBucketListHash));

    CLOG_INFO(History, "Caught up: want Bucket0Hash[{}] = {}", i,
              hexAbbrev(wantBucket0Hash));
    CLOG_INFO(History, "Caught up: have Bucket0Hash[{}] = {}", i,
              hexAbbrev(haveBucket0Hash));

    CLOG_INFO(History, "Caught up: want Bucket1Hash[{}] = {}", i,
              hexAbbrev(wantBucket1Hash));
    CLOG_INFO(History, "Caught up: have Bucket1Hash[{}] = {}", i,
              hexAbbrev(haveBucket1Hash));

    CHECK(nextLedger == haveSeq + 1);
    CHECK(wantSeq == haveSeq);
    CHECK(wantBucketListHash == haveBucketListHash);
    CHECK(wantHash == haveHash);

    CHECK(app->getBucketManager().getBucketByHash<LiveBucket>(wantBucket0Hash));
    CHECK(app->getBucketManager().getBucketByHash<LiveBucket>(wantBucket1Hash));
    CHECK(wantBucket0Hash == haveBucket0Hash);
    CHECK(wantBucket1Hash == haveBucket1Hash);

    auto haveRootBalance = rootBalances.at(i);
    auto haveAliceBalance = aliceBalances.at(i);
    auto haveBobBalance = bobBalances.at(i);
    auto haveCarolBalance = carolBalances.at(i);
    auto haveEveBalance = eveBalances.at(i);
    auto haveStrpBalance = stroopyBalances.at(i);

    auto haveRootSeq = rootSeqs.at(i);
    auto haveAliceSeq = aliceSeqs.at(i);
    auto haveBobSeq = bobSeqs.at(i);
    auto haveCarolSeq = carolSeqs.at(i);
    auto haveEveSeq = eveSeqs.at(i);
    auto haveStrpSeq = stroopySeqs.at(i);

    auto wantRootBalance = root.getBalance();
    auto wantAliceBalance = alice.getBalance();
    auto wantBobBalance = bob.getBalance();
    auto wantCarolBalance = carol.getBalance();
    auto wantEveBalance = eve.getBalance();
    auto wantStrpBalance = stroopy.getBalance();

    auto wantRootSeq = root.loadSequenceNumber();
    auto wantAliceSeq = alice.loadSequenceNumber();
    auto wantBobSeq = bob.loadSequenceNumber();
    auto wantCarolSeq = carol.loadSequenceNumber();
    auto wantEveSeq = eve.loadSequenceNumber();
    auto wantStrpSeq = stroopy.loadSequenceNumber();

    CHECK(haveRootBalance == wantRootBalance);
    CHECK(haveAliceBalance == wantAliceBalance);
    CHECK(haveBobBalance == wantBobBalance);
    CHECK(haveCarolBalance == wantCarolBalance);
    CHECK(haveEveBalance == wantEveBalance);
    CHECK(haveStrpBalance == wantStrpBalance);

    CHECK(haveRootSeq == wantRootSeq);
    CHECK(haveAliceSeq == wantAliceSeq);
    CHECK(haveBobSeq == wantBobSeq);
    CHECK(haveCarolSeq == wantCarolSeq);
    CHECK(haveEveSeq == wantEveSeq);
    CHECK(haveStrpSeq == wantStrpSeq);
}

CatchupPerformedWork
CatchupSimulation::computeCatchupPerformedWork(
    uint32_t lastClosedLedger, CatchupConfiguration const& catchupConfiguration,
    Application& app)
{
    auto const& hm = app.getHistoryManager();

    auto catchupRange =
        CatchupRange{lastClosedLedger, catchupConfiguration, hm};
    auto verifyCheckpointRange =
        CheckpointRange{catchupRange.getFullRangeIncludingBucketApply(), hm};

    uint32_t historyArchiveStatesDownloaded = 1;
    if (catchupRange.applyBuckets() && verifyCheckpointRange.mCount > 1)
    {
        historyArchiveStatesDownloaded++;
    }

    auto checkpointsDownloaded = verifyCheckpointRange.mCount;
    uint32_t txSetsDownloaded;
    if (catchupRange.replayLedgers())
    {
        auto applyCheckpointRange =
            CheckpointRange{catchupRange.getReplayRange(), hm};
        txSetsDownloaded = applyCheckpointRange.mCount;
    }
    else
    {
        txSetsDownloaded = 0;
    }

    auto firstVerifiedLedger =
        std::max(LedgerManager::GENESIS_LEDGER_SEQ,
                 verifyCheckpointRange.mFirst + 1 -
                     HistoryManager::getCheckpointFrequency(app.getConfig()));
    auto ledgersVerified =
        catchupConfiguration.toLedger() - firstVerifiedLedger + 1;
    auto txSetsApplied = catchupRange.getReplayCount();
    return {historyArchiveStatesDownloaded,
            checkpointsDownloaded,
            ledgersVerified,
            0,
            catchupRange.applyBuckets(),
            catchupRange.applyBuckets(),
            txSetsDownloaded,
            txSetsApplied};
}

void
CatchupSimulation::restartApp()
{
    mAppPtr.reset();
    mClock = std::make_unique<VirtualClock>(mClock->getMode());
    mAppPtr = createTestApplication(*mClock, mCfg, /*newDB*/ false);
}
}
}
