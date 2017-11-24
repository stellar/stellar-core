// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "history/HistoryTestsUtils.h"
#include "bucket/BucketManager.h"
#include "crypto/Hex.h"
#include "herder/TxSetFrame.h"
#include "ledger/CheckpointRange.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "work/WorkManager.h"

#include <medida/metrics_registry.h>

using namespace stellar;
using namespace txtest;

namespace stellar
{
using xdr::operator==;

namespace historytestutils
{

std::string
HistoryConfigurator::getArchiveDirName() const
{
    return "";
}

TmpDirHistoryConfigurator::TmpDirHistoryConfigurator()
    : mArchtmp("archtmp"), mDir(mArchtmp.tmpDir("archive"))
{
}

std::string
TmpDirHistoryConfigurator::getArchiveDirName() const
{
    return mDir.getName();
}

Config&
TmpDirHistoryConfigurator::configure(Config& mCfg, bool writable) const
{
    std::string d = mDir.getName();
    std::string getCmd = "cp " + d + "/{0} {1}";
    std::string putCmd = "";
    std::string mkdirCmd = "";

    if (writable)
    {
        putCmd = "cp {0} " + d + "/{1}";
        mkdirCmd = "mkdir -p " + d + "/{0}";
    }

    mCfg.HISTORY["test"] =
        std::make_shared<HistoryArchive>("test", getCmd, putCmd, mkdirCmd);
    return mCfg;
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
        std::make_shared<HistoryArchive>("test", getCmd, putCmd, mkdirCmd);
    return mCfg;
}

CatchupMetrics::CatchupMetrics()
    : mHistoryArchiveStatesDownloaded{0}
    , mLedgersDownloaded{0}
    , mLedgersVerified{0}
    , mLedgerChainsVerificationFailed{0}
    , mBucketsDownloaded{false}
    , mBucketsApplied{false}
    , mTransactionsDownloaded{0}
    , mTransactionsApplied{0}
{
}

CatchupMetrics::CatchupMetrics(
    uint64_t historyArchiveStatesDownloaded, uint64_t ledgersDownloaded,
    uint64_t ledgersVerified, uint64_t ledgerChainsVerificationFailed,
    uint64_t bucketsDownloaded, uint64_t bucketsApplied,
    uint64_t transactionsDownloaded, uint64_t transactionsApplied)
    : mHistoryArchiveStatesDownloaded{historyArchiveStatesDownloaded}
    , mLedgersDownloaded{ledgersDownloaded}
    , mLedgersVerified{ledgersVerified}
    , mLedgerChainsVerificationFailed{ledgerChainsVerificationFailed}
    , mBucketsDownloaded{bucketsDownloaded}
    , mBucketsApplied{bucketsApplied}
    , mTransactionsDownloaded{transactionsDownloaded}
    , mTransactionsApplied{transactionsApplied}
{
}

CatchupMetrics
operator-(CatchupMetrics const& x, CatchupMetrics const& y)
{
    return CatchupMetrics{
        x.mHistoryArchiveStatesDownloaded - y.mHistoryArchiveStatesDownloaded,
        x.mLedgersDownloaded - y.mLedgersDownloaded,
        x.mLedgersVerified - y.mLedgersVerified,
        x.mLedgerChainsVerificationFailed - y.mLedgerChainsVerificationFailed,
        x.mBucketsDownloaded - y.mBucketsDownloaded,
        x.mBucketsApplied - y.mBucketsApplied,
        x.mTransactionsDownloaded - y.mTransactionsDownloaded,
        x.mTransactionsApplied - y.mTransactionsApplied};
}

CatchupPerformedWork::CatchupPerformedWork(CatchupMetrics const& metrics)
    : mHistoryArchiveStatesDownloaded{metrics.mHistoryArchiveStatesDownloaded}
    , mLedgersDownloaded{metrics.mLedgersDownloaded}
    , mLedgersVerified{metrics.mLedgersVerified}
    , mLedgerChainsVerificationFailed{metrics.mLedgerChainsVerificationFailed}
    , mBucketsDownloaded{metrics.mBucketsDownloaded > 0}
    , mBucketsApplied{metrics.mBucketsApplied > 0}
    , mTransactionsDownloaded{metrics.mTransactionsDownloaded}
    , mTransactionsApplied{metrics.mTransactionsApplied}
{
}

CatchupPerformedWork::CatchupPerformedWork(
    uint64_t historyArchiveStatesDownloaded, uint64_t ledgersDownloaded,
    uint64_t ledgersVerified, uint64_t ledgerChainsVerificationFailed,
    bool bucketsDownloaded, bool bucketsApplied,
    uint64_t transactionsDownloaded, uint64_t transactionsApplied)
    : mHistoryArchiveStatesDownloaded{historyArchiveStatesDownloaded}
    , mLedgersDownloaded{ledgersDownloaded}
    , mLedgersVerified{ledgersVerified}
    , mLedgerChainsVerificationFailed{ledgerChainsVerificationFailed}
    , mBucketsDownloaded{bucketsDownloaded}
    , mBucketsApplied{bucketsApplied}
    , mTransactionsDownloaded{transactionsDownloaded}
    , mTransactionsApplied{transactionsApplied}
{
}

bool
operator==(CatchupPerformedWork const& x, CatchupPerformedWork const& y)
{
    if (x.mHistoryArchiveStatesDownloaded != y.mHistoryArchiveStatesDownloaded)
    {
        return false;
    }
    if (x.mLedgersDownloaded != y.mLedgersDownloaded)
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
    if (x.mTransactionsDownloaded != y.mTransactionsDownloaded)
    {
        return false;
    }
    if (x.mTransactionsApplied != y.mTransactionsApplied)
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

CatchupSimulation::CatchupSimulation(std::shared_ptr<HistoryConfigurator> cg)
    : mHistoryConfigurator(cg)
    , mCfg(getTestConfig())
    , mAppPtr(createTestApplication(
          mClock, mHistoryConfigurator->configure(mCfg, true)))
    , mApp(*mAppPtr)
{
    CHECK(HistoryManager::initializeHistoryArchive(mApp, "test"));
}

CatchupSimulation::~CatchupSimulation()
{
}

void
CatchupSimulation::crankTillDone()
{
    while (!mApp.getWorkManager().allChildrenDone() &&
           !mApp.getClock().getIOService().stopped())
    {
        mApp.getClock().crank(true);
    }
}

void
CatchupSimulation::generateAndPublishInitialHistory(size_t nPublishes)
{
    mApp.start();

    auto& lm = mApp.getLedgerManager();

    // At this point LCL should be 1, current ledger should be 2
    REQUIRE(lm.getLastClosedLedgerHeader().header.ledgerSeq == 1);
    REQUIRE(lm.getCurrentLedgerHeader().ledgerSeq == 2);

    generateAndPublishHistory(nPublishes);
}

void
CatchupSimulation::generateRandomLedger()
{
    auto& lm = mApp.getLedgerManager();
    TxSetFramePtr txSet =
        std::make_shared<TxSetFrame>(lm.getLastClosedLedgerHeader().hash);

    uint32_t ledgerSeq = lm.getLedgerNum();
    uint64_t minBalance = lm.getMinBalance(5);
    uint64_t big = minBalance + ledgerSeq;
    uint64_t small = 100 + ledgerSeq;
    uint64_t closeTime = 60 * 5 * ledgerSeq;

    auto root = TestAccount{mApp, getRoot(mApp.getNetworkID())};
    auto alice = TestAccount{mApp, getAccount("alice")};
    auto bob = TestAccount{mApp, getAccount("bob")};
    auto carol = TestAccount{mApp, getAccount("carol")};

    // Root sends to alice every tx, bob every other tx, carol every 4rd tx.
    txSet->add(root.tx({createAccount(alice, big)}));
    txSet->add(root.tx({createAccount(bob, big)}));
    txSet->add(root.tx({createAccount(carol, big)}));
    txSet->add(root.tx({payment(alice, big)}));
    txSet->add(root.tx({payment(bob, big)}));
    txSet->add(root.tx({payment(carol, big)}));

    // They all randomly send a little to one another every ledger after #4
    if (ledgerSeq > 4)
    {
        if (flip())
            txSet->add(alice.tx({payment(bob, small)}));
        if (flip())
            txSet->add(alice.tx({payment(carol, small)}));

        if (flip())
            txSet->add(bob.tx({payment(alice, small)}));
        if (flip())
            txSet->add(bob.tx({payment(carol, small)}));

        if (flip())
            txSet->add(carol.tx({payment(alice, small)}));
        if (flip())
            txSet->add(carol.tx({payment(bob, small)}));
    }

    // Provoke sortForHash and hash-caching:
    txSet->getContentsHash();

    CLOG(DEBUG, "History") << "Closing synthetic ledger " << ledgerSeq
                           << " with " << txSet->size() << " txs (txhash:"
                           << hexAbbrev(txSet->getContentsHash()) << ")";

    StellarValue sv(txSet->getContentsHash(), closeTime, emptyUpgradeSteps, 0);
    mLedgerCloseDatas.emplace_back(ledgerSeq, txSet, sv);
    lm.closeLedger(mLedgerCloseDatas.back());

    mLedgerSeqs.push_back(lm.getLastClosedLedgerHeader().header.ledgerSeq);
    mLedgerHashes.push_back(lm.getLastClosedLedgerHeader().hash);
    mBucketListHashes.push_back(
        lm.getLastClosedLedgerHeader().header.bucketListHash);
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
CatchupSimulation::generateAndPublishHistory(size_t nPublishes)
{
    auto& lm = mApp.getLedgerManager();
    auto& hm = mApp.getHistoryManager();

    size_t publishSuccesses = hm.getPublishSuccessCount();
    SequenceNumber ledgerSeq = lm.getCurrentLedgerHeader().ledgerSeq;

    while (hm.getPublishSuccessCount() < (publishSuccesses + nPublishes))
    {
        uint64_t queueCount = hm.getPublishQueueCount();
        while (hm.getPublishQueueCount() == queueCount)
        {
            generateRandomLedger();
            ++ledgerSeq;
        }

        REQUIRE(lm.getCurrentLedgerHeader().ledgerSeq == ledgerSeq);

        // Advance until we've published (or failed to!)
        while (hm.getPublishSuccessCount() < hm.getPublishQueueCount())
        {
            REQUIRE(hm.getPublishFailureCount() == 0);
            mApp.getClock().crank(true);
        }
    }

    REQUIRE(hm.getPublishFailureCount() == 0);
    REQUIRE(hm.getPublishSuccessCount() == publishSuccesses + nPublishes);
    REQUIRE(lm.getLedgerNum() ==
            ((publishSuccesses + nPublishes) * hm.getCheckpointFrequency()));
}

Application::pointer
CatchupSimulation::catchupNewApplication(uint32_t initLedger, uint32_t count,
                                         bool manual, Config::TestDbMode dbMode,
                                         std::string const& appName)
{

    CLOG(INFO, "History") << "****";
    CLOG(INFO, "History") << "**** Beginning catchup test for app '" << appName
                          << "'";
    CLOG(INFO, "History") << "****";

    mCfgs.emplace_back(
        getTestConfig(static_cast<int>(mCfgs.size()) + 1, dbMode));
    mCfgs.back().CATCHUP_COMPLETE =
        count == std::numeric_limits<uint32_t>::max();
    if (count != std::numeric_limits<uint32_t>::max())
    {
        mCfgs.back().CATCHUP_RECENT = count;
    }
    Application::pointer app2 = createTestApplication(
        mClock, mHistoryConfigurator->configure(mCfgs.back(), false));

    app2->start();
    CHECK(catchupApplication(initLedger, count, manual, app2) == true);
    return app2;
}

bool
CatchupSimulation::catchupApplication(uint32_t initLedger, uint32_t count,
                                      bool manual, Application::pointer app2,
                                      bool doStart, uint32_t gap)
{
    auto startCatchupMetrics = getCatchupMetrics(app2);

    auto root = TestAccount{*app2, getRoot(mApp.getNetworkID())};
    auto alice = TestAccount{*app2, getAccount("alice")};
    auto bob = TestAccount{*app2, getAccount("bob")};
    auto carol = TestAccount{*app2, getAccount("carol")};

    auto& lm = app2->getLedgerManager();
    if (doStart)
    {
        // Normally Herder calls LedgerManager.externalizeValue(initLedger + 1)
        // and this _triggers_ catchup within the LM. However, we do this
        // out-of-order because we want to control the catchup mode rather than
        // let the LM pick it, and because we want to simulate a 1-ledger skew
        // between the publishing side and the catchup side so that the catchup
        // has "heard" exactly 1 consensus LedgerCloseData broadcast after the
        // event that triggered its catchup to begin.
        //
        // For example: we want initLedger to be (say) 191-or-less, so that it
        // catches up using block 3, but we want the publisher to advance past
        // 192 (the first entry in block 4) and externalize that value, so that
        // the catchup can see a {192}.prevHash to knit up block 3 against.

        CLOG(INFO, "History")
            << "force-starting catchup at initLedger=" << initLedger;

        lm.startCatchUp({initLedger, count}, manual);
    }

    // Push publishing side forward one-ledger into a history block if it's
    // sitting on the boundary of it. This will ensure there's something
    // externalizable to knit-up with on the catchup side.
    if (mApp.getHistoryManager().nextCheckpointLedger(
            mApp.getLedgerManager().getLastClosedLedgerNum()) ==
        mApp.getLedgerManager().getLedgerNum())
    {
        CLOG(INFO, "History")
            << "force-publishing first ledger in next history block, ledger="
            << mApp.getLedgerManager().getLedgerNum();
        generateRandomLedger();
    }

    // Externalize (to the catchup LM) the range of ledgers between initLedger
    // and as near as we can get to the first ledger of the block after
    // initLedger (inclusive), so that there's something to knit-up with. Do not
    // externalize anything we haven't yet published, of course.
    if (!manual)
    {
        uint32_t nextBlockStart =
            mApp.getHistoryManager().nextCheckpointLedger(initLedger);
        for (uint32_t n = initLedger + 1; n <= nextBlockStart; ++n)
        {
            // Remember the vectors count from 2, not 0.
            if (n - 2 >= mLedgerCloseDatas.size())
            {
                break;
            }
            if (n == gap)
            {
                CLOG(INFO, "History")
                    << "simulating LedgerClose transmit gap at ledger " << n;
            }
            else
            {
                // Remember the vectors count from 2, not 0.
                auto const& lcd = mLedgerCloseDatas.at(n - 2);
                CLOG(INFO, "History")
                    << "force-externalizing LedgerCloseData for " << n
                    << " has txhash:"
                    << hexAbbrev(lcd.getTxSet()->getContentsHash());
                lm.valueExternalized(lcd);
            }
        }
    }

    uint32_t lastLedger = lm.getLastClosedLedgerNum();
    auto catchupConfiguration = CatchupConfiguration(initLedger, count);

    REQUIRE(!app2->getClock().getIOService().stopped());

    while (!app2->getWorkManager().allChildrenDone())
    {
        app2->getClock().crank(false);
    }

    if (app2->getLedgerManager().getState() != LedgerManager::LM_SYNCED_STATE)
    {
        return false;
    }

    auto endCatchupMetrics = getCatchupMetrics(app2);
    auto catchupPerformedWork =
        CatchupPerformedWork{endCatchupMetrics - startCatchupMetrics};

    REQUIRE(catchupPerformedWork ==
            computeCatchupPerformedWork(lastLedger, catchupConfiguration,
                                        app2->getHistoryManager()));

    uint32_t nextLedger = lm.getLedgerNum();

    CLOG(INFO, "History") << "Caught up: lastLedger = " << lastLedger;
    CLOG(INFO, "History") << "Caught up: initLedger = " << initLedger;
    CLOG(INFO, "History") << "Caught up: nextLedger = " << nextLedger;
    CLOG(INFO, "History") << "Caught up: published range is "
                          << mLedgerSeqs.size() << " ledgers, covering "
                          << "[" << mLedgerSeqs.front() << ", "
                          << mLedgerSeqs.back() << "]";

    // Assuming we caught up to nextLedger 128 (say), LCL will be 127, so we
    // must subtract 1.
    //
    // The local history vectors are built starting from ledger 2 (put at
    // vector-entry 0), so
    // to access slot 127 we must subtract 2 more.
    //
    // So cumulatively: we want to probe local history slot i = nextLedger - 3.

    REQUIRE(nextLedger != 0);
    if (nextLedger >= 3)
    {
        size_t i = nextLedger - 3;

        auto wantSeq = mLedgerSeqs.at(i);
        auto wantHash = mLedgerHashes.at(i);
        auto wantBucketListHash = mBucketListHashes.at(i);
        auto wantBucket0Hash = mBucket0Hashes.at(i);
        auto wantBucket1Hash = mBucket1Hashes.at(i);

        auto haveSeq = lm.getLastClosedLedgerHeader().header.ledgerSeq;
        auto haveHash = lm.getLastClosedLedgerHeader().hash;
        auto haveBucketListHash =
            lm.getLastClosedLedgerHeader().header.bucketListHash;
        auto haveBucket0Hash = app2->getBucketManager()
                                   .getBucketList()
                                   .getLevel(0)
                                   .getCurr()
                                   ->getHash();
        auto haveBucket1Hash = app2->getBucketManager()
                                   .getBucketList()
                                   .getLevel(2)
                                   .getCurr()
                                   ->getHash();

        CLOG(INFO, "History")
            << "Caught up: want Seq[" << i << "] = " << wantSeq;
        CLOG(INFO, "History")
            << "Caught up: have Seq[" << i << "] = " << haveSeq;

        CLOG(INFO, "History")
            << "Caught up: want Hash[" << i << "] = " << hexAbbrev(wantHash);
        CLOG(INFO, "History")
            << "Caught up: have Hash[" << i << "] = " << hexAbbrev(haveHash);

        CLOG(INFO, "History") << "Caught up: want BucketListHash[" << i
                              << "] = " << hexAbbrev(wantBucketListHash);
        CLOG(INFO, "History") << "Caught up: have BucketListHash[" << i
                              << "] = " << hexAbbrev(haveBucketListHash);

        CLOG(INFO, "History") << "Caught up: want Bucket0Hash[" << i
                              << "] = " << hexAbbrev(wantBucket0Hash);
        CLOG(INFO, "History") << "Caught up: have Bucket0Hash[" << i
                              << "] = " << hexAbbrev(haveBucket0Hash);

        CLOG(INFO, "History") << "Caught up: want Bucket1Hash[" << i
                              << "] = " << hexAbbrev(wantBucket1Hash);
        CLOG(INFO, "History") << "Caught up: have Bucket1Hash[" << i
                              << "] = " << hexAbbrev(haveBucket1Hash);

        CHECK(nextLedger == haveSeq + 1);
        CHECK(wantSeq == haveSeq);
        CHECK(wantBucketListHash == haveBucketListHash);
        CHECK(wantHash == haveHash);

        CHECK(app2->getBucketManager().getBucketByHash(wantBucket0Hash));
        CHECK(app2->getBucketManager().getBucketByHash(wantBucket1Hash));
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

    mApp.getLedgerManager().checkDbState();
    return true;
}

CatchupMetrics
CatchupSimulation::getCatchupMetrics(Application::pointer app)
{
    auto& getHistoryArchiveStateSuccess = app->getMetrics().NewMeter(
        {"history", "download-history-archive-state", "success"}, "event");
    auto historyArchiveStatesDownloaded = getHistoryArchiveStateSuccess.count();

    auto& downloadLedgersCached = app->getMetrics().NewMeter(
        {"history", "download-ledger", "cached"}, "event");
    auto& downloadLedgersSuccess = app->getMetrics().NewMeter(
        {"history", "download-ledger", "success"}, "event");

    auto ledgersDownloaded =
        downloadLedgersSuccess.count() + downloadLedgersCached.count();

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

    auto& downloadTransactionsCached = app->getMetrics().NewMeter(
        {"history", "download-transactions", " cached "}, "event");
    auto& downloadTransactionsSuccess = app->getMetrics().NewMeter(
        {"history", "download-transactions", "success"}, "event");

    auto transactionsDownloaded = downloadTransactionsSuccess.count() +
                                  downloadTransactionsCached.count();

    auto& applyLedgerSuccess = app->getMetrics().NewMeter(
        {"history", "apply-ledger", "success"}, "event");

    auto transactionsApplied = applyLedgerSuccess.count();

    return CatchupMetrics{
        historyArchiveStatesDownloaded, ledgersDownloaded,  ledgersVerified,
        ledgerChainsVerificationFailed, bucketsDownloaded,  bucketsApplied,
        transactionsDownloaded,         transactionsApplied};
}

CatchupPerformedWork
CatchupSimulation::computeCatchupPerformedWork(
    uint32_t lastClosedLedger, CatchupConfiguration const& catchupConfiguration,
    HistoryManager const& historyManager)
{
    auto catchupRange = CatchupWork::makeCatchupRange(
        lastClosedLedger, catchupConfiguration, historyManager);
    auto checkpointRange = CheckpointRange{catchupRange.first, historyManager};

    uint32_t historyArchiveStatesDownloaded = 1;
    if (catchupRange.second &&
        checkpointRange.first() != checkpointRange.last())
    {
        historyArchiveStatesDownloaded++;
    }

    auto filesDownloaded = checkpointRange.count();
    auto firstVerifiedLedger = std::max(
        LedgerManager::GENESIS_LEDGER_SEQ,
        checkpointRange.first() + 1 - historyManager.getCheckpointFrequency());
    auto ledgersVerified =
        catchupConfiguration.toLedger() - firstVerifiedLedger + 1;
    auto transactionsApplied =
        catchupRange.second
            ? catchupConfiguration.toLedger() - checkpointRange.first()
            : catchupConfiguration.toLedger() - lastClosedLedger;
    return {historyArchiveStatesDownloaded,
            filesDownloaded,
            ledgersVerified,
            0,
            catchupRange.second,
            catchupRange.second,
            filesDownloaded,
            transactionsApplied};
}
}
}
