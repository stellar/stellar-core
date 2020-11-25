#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketList.h"
#include "catchup/VerifyLedgerChainWork.h"
#include "crypto/Hex.h"
#include "herder/HerderImpl.h"
#include "herder/LedgerCloseData.h"
#include "history/FileTransferInfo.h"
#include "history/HistoryArchive.h"
#include "historywork/GzipFileWork.h"
#include "historywork/MakeRemoteDirWork.h"
#include "historywork/PutRemoteFileWork.h"
#include "ledger/LedgerRange.h"
#include "ledger/test/LedgerTestUtils.h"
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
    std::string mName;
    TmpDirManager mArchtmp;

  public:
    TmpDirHistoryConfigurator();

    std::string getArchiveDirName() const override;

    Config& configure(Config& cfg, bool writable) const override;
};

class MultiArchiveHistoryConfigurator : public HistoryConfigurator
{
    std::vector<std::shared_ptr<TmpDirHistoryConfigurator>> mConfigurators;

  public:
    explicit MultiArchiveHistoryConfigurator(uint32_t numArchives = 2);

    Config& configure(Config& cfg, bool writable) const;

    std::vector<std::shared_ptr<TmpDirHistoryConfigurator>>
    getConfigurators() const
    {
        return mConfigurators;
    }
};

class RealGenesisTmpDirHistoryConfigurator : public TmpDirHistoryConfigurator
{
  public:
    Config& configure(Config& cfg, bool writable) const override;
};

class BucketOutputIteratorForTesting : public BucketOutputIterator
{
    const size_t NUM_ITEMS_PER_BUCKET = 5;

  public:
    explicit BucketOutputIteratorForTesting(std::string const& tmpDir,
                                            uint32_t protocolVersion,
                                            MergeCounters& mc,
                                            asio::io_context& ctx);
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
    uint64_t mCheckpointsDownloaded;
    uint64_t mLedgersVerified;
    uint64_t mLedgerChainsVerificationFailed;
    uint64_t mBucketsDownloaded;
    uint64_t mBucketsApplied;
    uint64_t mTxSetsDownloaded;
    uint64_t mTxSetsApplied;

    CatchupMetrics();

    CatchupMetrics(uint64_t historyArchiveStatesDownloaded,
                   uint64_t checkpointsDownloaded, uint64_t ledgersVerified,
                   uint64_t ledgerChainsVerificationFailed,
                   uint64_t bucketsDownloaded, uint64_t bucketsApplied,
                   uint64_t txSetsDownloaded, uint64_t txSetsApplied);

    friend CatchupMetrics operator-(CatchupMetrics const& x,
                                    CatchupMetrics const& y);
};

struct CatchupPerformedWork
{
    uint64_t mHistoryArchiveStatesDownloaded;
    uint64_t mCheckpointsDownloaded;
    uint64_t mLedgersVerified;
    uint64_t mLedgerChainsVerificationFailed;
    bool mBucketsDownloaded;
    bool mBucketsApplied;
    uint64_t mTxSetsDownloaded;
    uint64_t mTxSetsApplied;

    CatchupPerformedWork(CatchupMetrics const& metrics);

    CatchupPerformedWork(uint64_t historyArchiveStatesDownloaded,
                         uint64_t checkpointsDownloaded,
                         uint64_t ledgersVerified,
                         uint64_t ledgerChainsVerificationFailed,
                         bool bucketsDownloaded, bool bucketsApplied,
                         uint64_t txSetsDownloaded, uint64_t txSetsApplied);

    friend bool operator==(CatchupPerformedWork const& x,
                           CatchupPerformedWork const& y);
    friend bool operator!=(CatchupPerformedWork const& x,
                           CatchupPerformedWork const& y);
};

class CatchupSimulation
{
  protected:
    VirtualClock mClock;
    std::list<VirtualClock> mSpawnedAppsClocks;
    std::shared_ptr<HistoryConfigurator> mHistoryConfigurator;
    Config mCfg;
    std::vector<Config> mCfgs;
    Application::pointer mAppPtr;
    Application& mApp;
    BucketList mBucketListAtLastPublish;

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

    uint32_t mTestProtocolShadowsRemovedLedgerSeq{0};

    CatchupMetrics getCatchupMetrics(Application::pointer app);
    CatchupPerformedWork computeCatchupPerformedWork(
        uint32_t lastClosedLedger,
        CatchupConfiguration const& catchupConfiguration, Application& app);
    void validateCatchup(Application::pointer app);

  public:
    explicit CatchupSimulation(
        VirtualClock::Mode mode = VirtualClock::VIRTUAL_TIME,
        std::shared_ptr<HistoryConfigurator> cg =
            std::make_shared<TmpDirHistoryConfigurator>(),
        bool startApp = true);
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

    uint32_t getLastCheckpointLedger(uint32_t checkpointIndex) const;

    void generateRandomLedger(uint32_t version = 0);

    void ensurePublishesComplete();
    void ensureLedgerAvailable(uint32_t targetLedger);
    void ensureOfflineCatchupPossible(uint32_t targetLedger);
    void ensureOnlineCatchupPossible(uint32_t targetLedger,
                                     uint32_t bufferLedgers = 0);

    std::vector<LedgerNumHashPair> getAllPublishedCheckpoints() const;
    LedgerNumHashPair getLastPublishedCheckpoint() const;

    Application::pointer createCatchupApplication(uint32_t count,
                                                  Config::TestDbMode dbMode,
                                                  std::string const& appName,
                                                  bool publish = false);
    bool catchupOffline(Application::pointer app, uint32_t toLedger,
                        bool extraValidation = false);
    bool catchupOnline(Application::pointer app, uint32_t initLedger,
                       uint32_t bufferLedgers = 0, uint32_t gapLedger = 0,
                       int32_t numGapLedgers = 1,
                       std::vector<uint32_t> const& ledgersToInject = {});

    // this method externalizes through herder
    void externalizeLedger(HerderImpl& herder, uint32_t ledger);

    void crankUntil(Application::pointer app,
                    std::function<bool()> const& predicate,
                    VirtualClock::duration duration);

    void setProto12UpgradeLedger(uint32_t ledger);
};
}
}
