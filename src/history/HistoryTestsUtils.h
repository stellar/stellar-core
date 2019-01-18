#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "FileTransferInfo.h"
#include "bucket/BucketList.h"
#include "catchup/VerifyLedgerChainWork.h"
#include "crypto/Hex.h"
#include "herder/LedgerCloseData.h"
#include "history/HistoryArchive.h"
#include "historywork/GzipFileWork.h"
#include "historywork/MakeRemoteDirWork.h"
#include "historywork/PutRemoteFileWork.h"
#include "ledger/LedgerRange.h"
#include "ledger/LedgerTestUtils.h"
#include "main/Application.h"
#include "main/Config.h"
#include "util/Timer.h"
#include "util/TmpDir.h"

#include "bucket/BucketOutputIterator.h"
#include "ledger/CheckpointRange.h"
#include "lib/catch.hpp"
#include <random>

namespace stellar
{

class CatchupConfiguration;
class HistoryManager;

namespace historytestutils
{

enum class TestBucketState
{
    CONTENTS_AND_HASH_OK,
    CORRUPTED_ZIPPED_FILE,
    FILE_NOT_UPLOADED,
    HASH_MISMATCH
};

class HistoryConfigurator;
class TestBucketGenerator;
class BucketOutputIteratorForTesting;
struct CatchupMetrics;
struct CatchupPerformedWork;

class HistoryConfigurator : NonCopyable
{
  public:
    virtual ~HistoryConfigurator() = default;
    virtual Config& configure(Config& cfg, bool writable) const = 0;
    virtual std::string getArchiveDirName() const;
};

class S3HistoryConfigurator : public HistoryConfigurator
{
  public:
    virtual Config& configure(Config& cfg, bool writable) const override;
};

class TmpDirHistoryConfigurator : public HistoryConfigurator
{
    TmpDirManager mArchtmp;
    TmpDir mDir;

  public:
    TmpDirHistoryConfigurator();

    std::string getArchiveDirName() const override;

    Config& configure(Config& cfg, bool writable) const override;
};

class BucketOutputIteratorForTesting : public BucketOutputIterator
{
    const size_t NUM_ITEMS_PER_BUCKET = 5;

  public:
    explicit BucketOutputIteratorForTesting(std::string const& tmpDir);
    std::pair<std::string, uint256> writeTmpTestBucket();
};

class TestBucketGenerator
{
    Application& mApp;
    std::shared_ptr<TmpDir> mTmpDir;
    std::shared_ptr<HistoryArchive> mArchive;

  public:
    TestBucketGenerator(Application& app,
                        std::shared_ptr<HistoryArchive> archive);

    std::string generateBucket(
        TestBucketState desiredState = TestBucketState::CONTENTS_AND_HASH_OK);
};

class TestLedgerChainGenerator
{
    Application& mApp;
    std::shared_ptr<HistoryArchive> mArchive;
    CheckpointRange mCheckpointRange;
    TmpDir const& mTmpDir;

  public:
    using CheckpointEnds =
        std::pair<LedgerHeaderHistoryEntry, LedgerHeaderHistoryEntry>;
    TestLedgerChainGenerator(Application& app,
                             std::shared_ptr<HistoryArchive> archive,
                             CheckpointRange range, const TmpDir& tmpDir);
    void createHistoryFiles(std::vector<LedgerHeaderHistoryEntry> const& lhv,
                            LedgerHeaderHistoryEntry& first,
                            LedgerHeaderHistoryEntry& last,
                            uint32_t checkpoint);
    CheckpointEnds
    makeOneLedgerFile(uint32_t currCheckpoint, Hash prevHash,
                      HistoryManager::LedgerVerificationStatus state);
    CheckpointEnds
    makeLedgerChainFiles(HistoryManager::LedgerVerificationStatus state =
                             HistoryManager::VERIFY_STATUS_OK);
};

struct CatchupMetrics
{
    uint64_t mHistoryArchiveStatesDownloaded;
    uint64_t mLedgersDownloaded;
    uint64_t mLedgersVerified;
    uint64_t mLedgerChainsVerificationFailed;
    uint64_t mBucketsDownloaded;
    uint64_t mBucketsApplied;
    uint64_t mTransactionsDownloaded;
    uint64_t mTransactionsApplied;

    CatchupMetrics();

    CatchupMetrics(uint64_t historyArchiveStatesDownloaded,
                   uint64_t ledgersDownloaded, uint64_t ledgersVerified,
                   uint64_t ledgerChainsVerificationFailed,
                   uint64_t bucketsDownloaded, uint64_t bucketsApplied,
                   uint64_t transactionsDownloaded,
                   uint64_t transactionsApplied);

    friend CatchupMetrics operator-(CatchupMetrics const& x,
                                    CatchupMetrics const& y);
};

struct CatchupPerformedWork
{
    uint64_t mHistoryArchiveStatesDownloaded;
    uint64_t mLedgersDownloaded;
    uint64_t mLedgersVerified;
    uint64_t mLedgerChainsVerificationFailed;
    bool mBucketsDownloaded;
    bool mBucketsApplied;
    uint64_t mTransactionsDownloaded;
    uint64_t mTransactionsApplied;

    CatchupPerformedWork(CatchupMetrics const& metrics);

    CatchupPerformedWork(uint64_t historyArchiveStatesDownloaded,
                         uint64_t ledgersDownloaded, uint64_t ledgersVerified,
                         uint64_t ledgerChainsVerificationFailed,
                         bool bucketsDownloaded, bool bucketsApplied,
                         uint64_t transactionsDownloaded,
                         uint64_t transactionsApplied);

    friend bool operator==(CatchupPerformedWork const& x,
                           CatchupPerformedWork const& y);
    friend bool operator!=(CatchupPerformedWork const& x,
                           CatchupPerformedWork const& y);
};

class CatchupSimulation
{
  protected:
    VirtualClock mClock;
    std::shared_ptr<HistoryConfigurator> mHistoryConfigurator;
    Config mCfg;
    std::vector<Config> mCfgs;
    Application::pointer mAppPtr;
    Application& mApp;
    BucketList mBucketListAtLastPublish;

    std::default_random_engine mGenerator;
    std::bernoulli_distribution mFlip{0.5};

    std::vector<LedgerCloseData> mLedgerCloseDatas;

    std::vector<uint32_t> mLedgerSeqs;
    std::vector<uint256> mLedgerHashes;
    std::vector<uint256> mBucketListHashes;
    std::vector<uint256> mBucket0Hashes;
    std::vector<uint256> mBucket1Hashes;

    std::vector<int64_t> rootBalances;
    std::vector<int64_t> aliceBalances;
    std::vector<int64_t> bobBalances;
    std::vector<int64_t> carolBalances;

    std::vector<SequenceNumber> rootSeqs;
    std::vector<SequenceNumber> aliceSeqs;
    std::vector<SequenceNumber> bobSeqs;
    std::vector<SequenceNumber> carolSeqs;

  public:
    explicit CatchupSimulation(
        std::shared_ptr<HistoryConfigurator> cg =
            std::make_shared<TmpDirHistoryConfigurator>());
    ~CatchupSimulation();

    Application&
    getApp() const
    {
        return mApp;
    }

    VirtualClock&
    getClock()
    {
        return mClock;
    }

    HistoryConfigurator&
    getHistoryConfigurator() const
    {
        return *mHistoryConfigurator.get();
    }

    BucketList
    getBucketListAtLastPublish() const
    {
        return mBucketListAtLastPublish;
    }

    void generateRandomLedger();
    void generateAndPublishHistory(size_t nPublishes);
    void generateAndPublishInitialHistory(size_t nPublishes);

    Application::pointer catchupNewApplication(uint32_t initLedger,
                                               uint32_t count, bool manual,
                                               Config::TestDbMode dbMode,
                                               std::string const& appName);
    bool catchupApplication(uint32_t initLedger, uint32_t count, bool manual,
                            Application::pointer app2, uint32_t gap = 0);

    CatchupMetrics getCatchupMetrics(Application::pointer app);
    CatchupPerformedWork computeCatchupPerformedWork(
        uint32_t lastClosedLedger,
        CatchupConfiguration const& catchupConfiguration,
        HistoryManager const& historyManager);

    void crankUntil(Application::pointer app,
                    std::function<bool()> const& predicate,
                    VirtualClock::duration duration);

    bool
    flip()
    {
        return mFlip(mGenerator);
    }
};
}
}
