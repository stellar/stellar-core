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

#include <medida/metrics_registry.h>

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
        LedgerTestUtils::generateValidLedgerEntries(NUM_ITEMS_PER_BUCKET);
    auto bucketEntries =
        Bucket::convertToBucketEntry(false, {}, ledgerEntries, {});
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

    return std::pair<std::string, uint256>(mFilename, mHasher.finish());
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
    uint256 hash = HashUtils::random();
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
        hash = HashUtils::random();
    }

    // Upload generated bucket to the archive
    {
        FileTransferInfo ft{mTmpDir->getName(), HISTORY_FILE_TYPE_BUCKET,
                            binToHex(hash)};
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
    FileTransferInfo ft{mTmpDir, HISTORY_FILE_TYPE_LEDGER, checkpoint};
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
    auto& hm = mApp.getHistoryManager();
    auto initLedger = hm.firstLedgerInCheckpointContaining(currCheckpoint);
    auto size = hm.sizeOfCheckpointContaining(currCheckpoint);

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
    Hash hash = HashUtils::random();
    LedgerHeaderHistoryEntry beginRange;

    LedgerHeaderHistoryEntry first, last;
    for (auto i = mCheckpointRange.mFirst; i < mCheckpointRange.limit();
         i += mApp.getHistoryManager().sizeOfCheckpointContaining(i))
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

CatchupMetrics::CatchupMetrics()
    : mHistoryArchiveStatesDownloaded{0}
    , mCheckpointsDownloaded{0}
    , mLedgersVerified{0}
    , mLedgerChainsVerificationFailed{0}
    , mBucketsDownloaded{false}
    , mBucketsApplied{false}
    , mTxSetsDownloaded{0}
    , mTxSetsApplied{0}
{
}

CatchupMetrics::CatchupMetrics(
    uint64_t historyArchiveStatesDownloaded, uint64_t checkpointsDownloaded,
    uint64_t ledgersVerified, uint64_t ledgerChainsVerificationFailed,
    uint64_t bucketsDownloaded, uint64_t bucketsApplied,
    uint64_t txSetsDownloaded, uint64_t txSetsApplied)
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

CatchupMetrics
operator-(CatchupMetrics const& x, CatchupMetrics const& y)
{
    return CatchupMetrics{
        x.mHistoryArchiveStatesDownloaded - y.mHistoryArchiveStatesDownloaded,
        x.mCheckpointsDownloaded - y.mCheckpointsDownloaded,
        x.mLedgersVerified - y.mLedgersVerified,
        x.mLedgerChainsVerificationFailed - y.mLedgerChainsVerificationFailed,
        x.mBucketsDownloaded - y.mBucketsDownloaded,
        x.mBucketsApplied - y.mBucketsApplied,
        x.mTxSetsDownloaded - y.mTxSetsDownloaded,
        x.mTxSetsApplied - y.mTxSetsApplied};
}

CatchupPerformedWork::CatchupPerformedWork(CatchupMetrics const& metrics)
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
                                     bool startApp)
    : mClock(mode)
    , mHistoryConfigurator(cg)
    , mCfg(getTestConfig())
    , mAppPtr(createTestApplication(
          mClock, mHistoryConfigurator->configure(mCfg, true)))
    , mApp(*mAppPtr)
{
    auto dirName = cg->getArchiveDirName();
    if (!dirName.empty())
    {
        CHECK(
            mApp.getHistoryArchiveManager().initializeHistoryArchive(dirName));
    }
    if (startApp)
    {
        mApp.start();
    }
}

CatchupSimulation::~CatchupSimulation()
{
}

uint32_t
CatchupSimulation::getLastCheckpointLedger(uint32_t checkpointIndex) const
{
    return mApp.getHistoryManager().getCheckpointFrequency() * checkpointIndex -
           1;
}

void
CatchupSimulation::generateRandomLedger(uint32_t version)
{
    auto& lm = mApp.getLedgerManager();
    TxSetFramePtr txSet =
        std::make_shared<TxSetFrame>(lm.getLastClosedLedgerHeader().hash);

    uint32_t ledgerSeq = lm.getLastClosedLedgerNum() + 1;
    uint64_t minBalance = lm.getLastMinBalance(5);
    uint64_t big = minBalance + ledgerSeq;
    uint64_t small = 100 + ledgerSeq;
    uint64_t closeTime = 60 * 5 * ledgerSeq;

    auto root = TestAccount{mApp, getRoot(mApp.getNetworkID())};
    auto alice = TestAccount{mApp, getAccount("alice")};
    auto bob = TestAccount{mApp, getAccount("bob")};
    auto carol = TestAccount{mApp, getAccount("carol")};

    // Root sends to alice every tx, bob every other tx, carol every 4rd tx.
    if (ledgerSeq < 5)
    {
        txSet->add(root.tx({createAccount(alice, big)}));
        txSet->add(root.tx({createAccount(bob, big)}));
        txSet->add(root.tx({createAccount(carol, big)}));
    }
    // Allow an occasional empty ledger
    else if (rand_flip() || rand_flip())
    {
        txSet->add(root.tx({payment(alice, big)}));
        txSet->add(root.tx({payment(bob, big)}));
        txSet->add(root.tx({payment(carol, big)}));

        // They all randomly send a little to one another every ledger after #4
        if (rand_flip())
            txSet->add(alice.tx({payment(bob, small)}));
        if (rand_flip())
            txSet->add(alice.tx({payment(carol, small)}));

        if (rand_flip())
            txSet->add(bob.tx({payment(alice, small)}));
        if (rand_flip())
            txSet->add(bob.tx({payment(carol, small)}));

        if (rand_flip())
            txSet->add(carol.tx({payment(alice, small)}));
        if (rand_flip())
            txSet->add(carol.tx({payment(bob, small)}));
    }

    // Provoke sortForHash and hash-caching:
    txSet->getContentsHash();

    CLOG_DEBUG(History, "Closing synthetic ledger {} with {} txs (txhash:{})",
               ledgerSeq, txSet->sizeTx(), hexAbbrev(txSet->getContentsHash()));

    auto upgrades = xdr::xvector<UpgradeType, 6>{};
    if (version > 0)
    {
        auto ledgerUpgrade = LedgerUpgrade{LEDGER_UPGRADE_VERSION};
        ledgerUpgrade.newLedgerVersion() = version;
        auto v = xdr::xdr_to_opaque(ledgerUpgrade);
        upgrades.push_back(UpgradeType{v.begin(), v.end()});
    }

    StellarValue sv =
        mApp.getHerder().makeStellarValue(txSet->getContentsHash(), closeTime,
                                          upgrades, mApp.getConfig().NODE_SEED);

    mLedgerCloseDatas.emplace_back(ledgerSeq, txSet, sv);
    lm.closeLedger(mLedgerCloseDatas.back());

    auto const& lclh = lm.getLastClosedLedgerHeader();
    mLedgerSeqs.push_back(lclh.header.ledgerSeq);
    mLedgerHashes.push_back(lclh.hash);
    mBucketListHashes.push_back(lclh.header.bucketListHash);
    mBucket0Hashes.push_back(mApp.getBucketManager()
                                 .getBucketList()
                                 .getLevel(0)
                                 .getCurr()
                                 ->getHash());
    mBucket1Hashes.push_back(mApp.getBucketManager()
                                 .getBucketList()
                                 .getLevel(2)
                                 .getCurr()
                                 ->getHash());

    rootBalances.push_back(root.getBalance());
    aliceBalances.push_back(alice.getBalance());
    bobBalances.push_back(bob.getBalance());
    carolBalances.push_back(carol.getBalance());

    rootSeqs.push_back(root.loadSequenceNumber());
    aliceSeqs.push_back(alice.loadSequenceNumber());
    bobSeqs.push_back(bob.loadSequenceNumber());
    carolSeqs.push_back(carol.loadSequenceNumber());
}

void
CatchupSimulation::setProto12UpgradeLedger(uint32_t ledger)
{
    REQUIRE(mApp.getLedgerManager().getLastClosedLedgerNum() < ledger);
    mTestProtocolShadowsRemovedLedgerSeq = ledger;
}

void
CatchupSimulation::ensureLedgerAvailable(uint32_t targetLedger)
{
    auto& lm = mApp.getLedgerManager();
    auto& hm = mApp.getHistoryManager();
    while (lm.getLastClosedLedgerNum() < targetLedger)
    {
        auto lcl = lm.getLastClosedLedgerNum();
        if (lcl + 1 == mTestProtocolShadowsRemovedLedgerSeq)
        {
            // Force proto 12 upgrade
            generateRandomLedger(Bucket::FIRST_PROTOCOL_SHADOWS_REMOVED);
        }
        else
        {
            generateRandomLedger();
        }

        if (hm.publishCheckpointOnLedgerClose(lcl))
        {
            mBucketListAtLastPublish =
                getApp().getBucketManager().getBucketList();
        }
    }
}

void
CatchupSimulation::ensurePublishesComplete()
{
    auto& hm = mApp.getHistoryManager();
    while (!mApp.getWorkScheduler().allChildrenDone() ||
           (hm.getPublishSuccessCount() < hm.getPublishQueueCount()))
    {
        REQUIRE(hm.getPublishFailureCount() == 0);
        mApp.getClock().crank(true);
    }

    REQUIRE(hm.getPublishFailureCount() == 0);
    // Make sure all references to buckets were released
    REQUIRE(hm.getBucketsReferencedByPublishQueue().empty());
}

void
CatchupSimulation::ensureOfflineCatchupPossible(uint32_t targetLedger)
{
    auto& hm = mApp.getHistoryManager();

    // One additional ledger is needed for publish.
    ensureLedgerAvailable(hm.checkpointContainingLedger(targetLedger) + 1);
    ensurePublishesComplete();
}

void
CatchupSimulation::ensureOnlineCatchupPossible(uint32_t targetLedger,
                                               uint32_t bufferLedgers)
{
    auto& hm = mApp.getHistoryManager();

    // One additional ledger is needed for publish, one as a trigger ledger for
    // catchup, one as closing ledger.
    ensureLedgerAvailable(hm.checkpointContainingLedger(targetLedger) +
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
    auto const& hm = mApp.getHistoryManager();
    while (si != mLedgerSeqs.end())
    {
        if (hm.isLastLedgerInCheckpoint(*si))
        {
            LedgerNumHashPair pair;
            pair.first = *si;
            pair.second = make_optional<Hash>(*hi);
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
    auto const& hm = mApp.getHistoryManager();
    while (si != mLedgerSeqs.rend())
    {
        if (hm.isLastLedgerInCheckpoint(*si))
        {
            pair.first = *si;
            pair.second = make_optional<Hash>(*hi);
            break;
        }
        ++hi;
        ++si;
    }
    return pair;
}

void
CatchupSimulation::crankUntil(Application::pointer app,
                              std::function<bool()> const& predicate,
                              VirtualClock::duration timeout)
{
    auto start = std::chrono::system_clock::now();
    while (!predicate())
    {
        app->getClock().crank(false);
        auto current = std::chrono::system_clock::now();
        auto diff = current - start;
        if (diff > timeout)
        {
            break;
        }
    }
}

Application::pointer
CatchupSimulation::createCatchupApplication(uint32_t count,
                                            Config::TestDbMode dbMode,
                                            std::string const& appName,
                                            bool publish)
{
    CLOG_INFO(History, "****");
    CLOG_INFO(History, "**** Create app for catchup: '{}'", appName);
    CLOG_INFO(History, "****");

    mCfgs.emplace_back(
        getTestConfig(static_cast<int>(mCfgs.size()) + 1, dbMode));
    mCfgs.back().CATCHUP_COMPLETE =
        count == std::numeric_limits<uint32_t>::max();
    mCfgs.back().CATCHUP_RECENT = count;
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

    auto startCatchupMetrics = getCatchupMetrics(app);
    auto& lm = app->getLedgerManager();
    auto lastLedger = lm.getLastClosedLedgerNum();
    auto mode = extraValidation ? CatchupConfiguration::Mode::OFFLINE_COMPLETE
                                : CatchupConfiguration::Mode::OFFLINE_BASIC;
    auto catchupConfiguration =
        CatchupConfiguration{toLedger, app->getConfig().CATCHUP_RECENT, mode};
    lm.startCatchup(catchupConfiguration, nullptr);
    REQUIRE(!app->getClock().getIOContext().stopped());

    auto& cm = app->getCatchupManager();
    auto finished = [&]() { return cm.catchupWorkIsDone(); };

    auto expectedCatchupWork =
        computeCatchupPerformedWork(lastLedger, catchupConfiguration, *app);
    crankUntil(app, finished,
               std::chrono::seconds{std::max<int64>(
                   expectedCatchupWork.mTxSetsApplied + 15, 60)});

    // Finished successfully
    auto success = cm.isCatchupInitialized() &&
                   cm.getCatchupWorkState() == BasicWork::State::WORK_SUCCESS;
    if (success)
    {
        CLOG_INFO(History, "Caught up");

        auto endCatchupMetrics = getCatchupMetrics(app);
        auto catchupPerformedWork =
            CatchupPerformedWork{endCatchupMetrics - startCatchupMetrics};

        REQUIRE(catchupPerformedWork == expectedCatchupWork);
        if (app->getHistoryArchiveManager().hasAnyWritableHistoryArchive())
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
    auto startCatchupMetrics = getCatchupMetrics(app);

    auto& hm = app->getHistoryManager();
    auto& herder = static_cast<HerderImpl&>(app->getHerder());

    // catchup will run to the final ledger in the checkpoint
    auto toLedger = hm.checkpointContainingLedger(initLedger - 1);

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
    if (hm.isFirstLedgerInCheckpoint(initLedger))
    {
        firstLedgerInCheckpoint = initLedger;
    }
    else
    {
        firstLedgerInCheckpoint =
            hm.firstLedgerAfterCheckpointContaining(initLedger);
    }

    uint32_t triggerLedger = hm.ledgerToTriggerCatchup(firstLedgerInCheckpoint);

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
        return app->getCatchupManager().catchupWorkIsDone();
    };

    auto lastLedger = lm.getLastClosedLedgerNum();

    auto expectedCatchupWork =
        computeCatchupPerformedWork(lastLedger, catchupConfiguration, *app);

    crankUntil(app, catchupIsDone,
               std::chrono::seconds{std::max<int64>(
                   expectedCatchupWork.mTxSetsApplied + 15, 60)});

    if (lm.getLastClosedLedgerNum() == triggerLedger + bufferLedgers)
    {
        // Externalize closing ledger
        externalize(triggerLedger + bufferLedgers + 1);
    }

    auto result = caughtUp();
    if (result)
    {
        REQUIRE(lm.getLastClosedLedgerNum() ==
                triggerLedger + bufferLedgers + 1);

        auto endCatchupMetrics = getCatchupMetrics(app);
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

    auto txSet = std::static_pointer_cast<TxSetFrame>(lcd.getTxSet());

    herder.getPendingEnvelopes().putTxSet(lcd.getTxSet()->getContentsHash(),
                                          lcd.getLedgerSeq(), txSet);
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

    auto root = TestAccount{*app, getRoot(mApp.getNetworkID())};
    auto alice = TestAccount{*app, getAccount("alice")};
    auto bob = TestAccount{*app, getAccount("bob")};
    auto carol = TestAccount{*app, getAccount("carol")};

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
                               .getBucketList()
                               .getLevel(0)
                               .getCurr()
                               ->getHash();
    auto haveBucket1Hash = app->getBucketManager()
                               .getBucketList()
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

    CHECK(app->getBucketManager().getBucketByHash(wantBucket0Hash));
    CHECK(app->getBucketManager().getBucketByHash(wantBucket1Hash));
    CHECK(wantBucket0Hash == haveBucket0Hash);
    CHECK(wantBucket1Hash == haveBucket1Hash);

    auto haveRootBalance = rootBalances.at(i);
    auto haveAliceBalance = aliceBalances.at(i);
    auto haveBobBalance = bobBalances.at(i);
    auto haveCarolBalance = carolBalances.at(i);

    auto haveRootSeq = rootSeqs.at(i);
    auto haveAliceSeq = aliceSeqs.at(i);
    auto haveBobSeq = bobSeqs.at(i);
    auto haveCarolSeq = carolSeqs.at(i);

    auto wantRootBalance = root.getBalance();
    auto wantAliceBalance = alice.getBalance();
    auto wantBobBalance = bob.getBalance();
    auto wantCarolBalance = carol.getBalance();

    auto wantRootSeq = root.loadSequenceNumber();
    auto wantAliceSeq = alice.loadSequenceNumber();
    auto wantBobSeq = bob.loadSequenceNumber();
    auto wantCarolSeq = carol.loadSequenceNumber();

    CHECK(haveRootBalance == wantRootBalance);
    CHECK(haveAliceBalance == wantAliceBalance);
    CHECK(haveBobBalance == wantBobBalance);
    CHECK(haveCarolBalance == wantCarolBalance);

    CHECK(haveRootSeq == wantRootSeq);
    CHECK(haveAliceSeq == wantAliceSeq);
    CHECK(haveBobSeq == wantBobSeq);
    CHECK(haveCarolSeq == wantCarolSeq);
}

CatchupMetrics
CatchupSimulation::getCatchupMetrics(Application::pointer app)
{
    auto& getHistoryArchiveStateSuccess = app->getMetrics().NewMeter(
        {"history", "download-history-archive-state", "success"}, "event");
    auto historyArchiveStatesDownloaded = getHistoryArchiveStateSuccess.count();

    // metric here is tracking checkpoints, not ledgers
    auto& checkpointsDownloadSuccess = app->getMetrics().NewMeter(
        {"history", "download-ledger", "success"}, "event");

    auto checkpointsDownloaded = checkpointsDownloadSuccess.count();

    auto& verifyLedgerSuccess = app->getMetrics().NewMeter(
        {"history", "verify-ledger", "success"}, "event");
    auto& verifyLedgerChainFailure = app->getMetrics().NewMeter(
        {"history", "verify-ledger-chain", "failure"}, "event");

    auto ledgersVerified = verifyLedgerSuccess.count();
    auto ledgerChainsVerificationFailed = verifyLedgerChainFailure.count();

    auto& downloadBucketSuccess = app->getMetrics().NewMeter(
        {"history", "download-bucket", "success"}, "event");

    auto bucketsDownloaded = downloadBucketSuccess.count();

    auto& bucketApplySuccess = app->getMetrics().NewMeter(
        {"history", "bucket-apply", "success"}, "event");

    auto bucketsApplied = bucketApplySuccess.count();

    // metric tracks transaction sets for each ledger
    auto& downloadTxSetsSuccess = app->getMetrics().NewMeter(
        {"history", "download-transactions", "success"}, "event");

    auto txSetsDownloaded = downloadTxSetsSuccess.count();

    auto& applyLedgerSuccess = app->getMetrics().NewMeter(
        {"history", "apply-ledger-chain", "success"}, "event");

    auto txSetsApplied = applyLedgerSuccess.count();

    return CatchupMetrics{historyArchiveStatesDownloaded,
                          checkpointsDownloaded,
                          ledgersVerified,
                          ledgerChainsVerificationFailed,
                          bucketsDownloaded,
                          bucketsApplied,
                          txSetsDownloaded,
                          txSetsApplied};
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

    auto firstVerifiedLedger = std::max(LedgerManager::GENESIS_LEDGER_SEQ,
                                        verifyCheckpointRange.mFirst + 1 -
                                            hm.getCheckpointFrequency());
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
}
}
