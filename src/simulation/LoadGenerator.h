#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SecretKey.h"
#include "herder/Herder.h"
#include "main/Application.h"
#include "test/TestAccount.h"
#include "test/TxTests.h"
#include "xdr/Stellar-types.h"
#include <vector>

namespace medida
{
class MetricsRegistry;
class Meter;
class Counter;
class Timer;
}

namespace stellar
{

class VirtualTimer;

enum class LoadGenMode
{
    CREATE,
    PAY,
    PRETEND,
    // Mix of payments and DEX-related transactions.
    MIXED_TXS,
    // Deploy random Wasm blobs, for overlay/herder testing
    SOROBAN_UPLOAD,
    // Deploy contracts to be used by SOROBAN_INVOKE
    SOROBAN_INVOKE_SETUP,
    // Invoke and apply resource intensive TXs, must run SOROBAN_INVOKE_SETUP
    // first
    SOROBAN_INVOKE,
};

struct GeneratedLoadConfig
{
    static GeneratedLoadConfig createAccountsLoad(uint32_t nAccounts,
                                                  uint32_t txRate);

    static GeneratedLoadConfig createSorobanInvokeSetupLoad(uint32_t nAccounts,
                                                            uint32_t txRate);

    static GeneratedLoadConfig
    txLoad(LoadGenMode mode, uint32_t nAccounts, uint32_t nTxs, uint32_t txRate,
           uint32_t offset = 0, std::optional<uint32_t> maxFee = std::nullopt);

    LoadGenMode mode = LoadGenMode::CREATE;
    uint32_t nAccounts = 0;
    uint32_t offset = 0;
    uint32_t nTxs = 0;
    // The number of transactions per second when there is no spike.
    uint32_t txRate = 0;
    // A spike will occur every spikeInterval seconds.
    // Set this to 0 if no spikes are needed.
    std::chrono::seconds spikeInterval = std::chrono::seconds(0);
    // The number of transactions a spike injects on top of the
    // steady rate.
    uint32_t spikeSize = 0;
    // When present, generate the transaction fees randomly with the fee rate up
    // to this value.
    // Does not affect account creation.
    std::optional<uint32_t> maxGeneratedFeeRate;
    // When true, skips when they're not accepted by Herder due to low fee (due
    // to `TxQueueLimiter` limiting the operation count per ledger). Otherwise,
    // the load generation will fail after a couple of retries.
    // Does not affect account creation.
    bool skipLowFeeTxs = false;
    // Percentage (from 0 to 100) of DEX transactions
    uint32_t dexTxPercent = 0;

    // The following ranges are only used for SOROBAN_INVOKE txs:
    uint32_t nInstances = 0;

    // Range of kilo bytes and num entries for disk IO
    uint32_t nDataEntriesLow = 0;
    uint32_t nDataEntriesHigh = 0;
    uint32_t kiloBytesPerDataEntryLow = 0;
    uint32_t kiloBytesPerDataEntryHigh = 0;

    // Size of transactions
    int32_t txSizeBytesLow = 0;
    int32_t txSizeBytesHigh = 0;

    // Instruction count
    uint64_t instructionsLow = 0;
    uint64_t instructionsHigh = 0;
};

class LoadGenerator
{
  public:
    using TestAccountPtr = std::shared_ptr<TestAccount>;
    LoadGenerator(Application& app);

    static LoadGenMode getMode(std::string const& mode);

    // Returns true if loadgen has submitted all required TXs
    bool isDone(GeneratedLoadConfig const& cfg) const;

    // Returns true if loadgen can continue and does not need to wait for Wasm
    // application
    bool checkSorobanWasmSetup(GeneratedLoadConfig const& cfg);

    // Generate one "step" worth of load (assuming 1 step per STEP_MSECS) at a
    // given target number of accounts and txs, a given target tx/s rate, and
    // according to the other parameters provided in configuration.
    // If work remains after the current step, calls scheduleLoadGeneration()
    // with the remainder.
    void generateLoad(GeneratedLoadConfig cfg);

    // Verify cached accounts are properly reflected in the database
    // return any accounts that are inconsistent.
    std::vector<TestAccountPtr> checkAccountSynced(Application& app,
                                                   bool isCreate);
    std::vector<LedgerKey> checkSorobanStateSynced(Application& app,
                                                   LoadGenMode mode);

    struct ContractInstance
    {
        // [wasm, instance]
        xdr::xvector<LedgerKey> readOnlyKeys;
        SCAddress contractID;
    };

    UnorderedSet<LedgerKey> const&
    getContractInstanceKeysForTesting() const
    {
        return mContractInstanceKeys;
    }

    std::optional<LedgerKey> const&
    getCodeKeyForTesting() const
    {
        return mCodeKey;
    }

  private:
    struct TxMetrics
    {
        medida::Meter& mAccountCreated;
        medida::Meter& mNativePayment;
        medida::Meter& mManageOfferOps;
        medida::Meter& mPretendOps;
        medida::Meter& mTxnAttempted;
        medida::Meter& mTxnRejected;
        medida::Meter& mTxnBytes;

        TxMetrics(medida::MetricsRegistry& m);
        void report();
    };

    // There are a few scenarios where tx submission might fail:
    // * ADD_STATUS_DUPLICATE, should be just a no-op and not count toward
    // total tx goal.
    // * ADD_STATUS_TRY_AGAIN_LATER, transaction is banned/dropped. This
    // indicates that the system is getting overloaded, so loadgen fails.
    // * ADD_STATUS_ERROR, transaction didn't pass validation checks. If
    // failure is due to txBAD_SEQ, synchronize accounts with the DB and
    // re-submit. Any other code points to a loadgen misconfigurations, as
    // transactions must have valid (pre-generated) source accounts,
    // sufficient balances etc.
    TransactionQueue::AddResult execute(TransactionFramePtr& txf,
                                        LoadGenMode mode,
                                        TransactionResultCode& code);
    TransactionFramePtr
    createTransactionFramePtr(TestAccountPtr from, std::vector<Operation> ops,
                              LoadGenMode mode,
                              std::optional<uint32_t> maxGeneratedFeeRate);

    static const std::chrono::milliseconds STEP_MSECS;
    static const uint32_t TX_SUBMIT_MAX_TRIES;
    static const uint32_t TIMEOUT_NUM_LEDGERS;
    static const uint32_t COMPLETION_TIMEOUT_WITHOUT_CHECKS;
    static const uint32_t MIN_UNIQUE_ACCOUNT_MULTIPLIER;

    std::unique_ptr<VirtualTimer> mLoadTimer;
    int64 mMinBalance;
    uint64_t mLastSecond;
    Application& mApp;
    int64_t mTotalSubmitted;
    // Set when load generation actually begins
    std::unique_ptr<VirtualClock::time_point> mStartTime;

    TestAccountPtr mRoot;
    // Accounts cache
    std::map<uint64_t, TestAccountPtr> mAccounts;

    // Track account IDs that are currently being referenced by the transaction
    // queue (to avoid source account collisions during tx submission)
    std::unordered_set<uint64_t> mAccountsInUse;
    std::unordered_set<uint64_t> mAccountsAvailable;
    uint64_t getNextAvailableAccount();

    // For account creation only: allocate a few accounts for creation purposes
    // (with sufficient balance to create new accounts) to avoid source account
    // collisions.
    std::unordered_map<uint64_t, TestAccountPtr> mCreationSourceAccounts;

    medida::Meter& mLoadgenComplete;
    medida::Meter& mLoadgenFail;

    bool mFailed{false};
    bool mStarted{false};
    bool mInitialAccountsCreated{false};

    uint32_t mWaitTillCompleteForLedgers{0};
    uint32_t mSorobanWasmWaitTillLedgers{0};

    // Internal loadgen state gets reset after each run, but it is impossible to
    // regenerate contract instance keys for DB lookup. Due to this we maintain
    // a static list of instances and the wasm entry which we use to rebuild
    // mContractInstances at the start of each SOROBAN_INVOKE run
    inline static UnorderedSet<LedgerKey> mContractInstanceKeys = {};

    // Wasm ledger entry is stored in mPendingCodeKey while TX is in flight.
    // Once TX has been successfully applied, mPendingCodeKey is moved to
    // mCodeKey.
    std::optional<LedgerKey> mPendingCodeKey{};
    inline static std::optional<LedgerKey> mCodeKey = std::nullopt;
    inline static uint64_t mCodeSize = 0;

    // Maps account ID to it's contract instance, where each account has a
    // unique instance
    UnorderedMap<uint64_t, ContractInstance> mContractInstances;

    void reset(bool resetSoroban);
    void createRootAccount();
    int64_t getTxPerStep(uint32_t txRate, std::chrono::seconds spikeInterval,
                         uint32_t spikeSize);

    // Schedule a callback to generateLoad() interval milliseconds from now.
    void scheduleLoadGeneration(GeneratedLoadConfig cfg,
                                std::chrono::milliseconds interval);

    std::vector<Operation> createAccounts(uint64_t i, uint64_t batchSize,
                                          uint32_t ledgerNum,
                                          bool initialAccounts);
    bool loadAccount(TestAccount& account, Application& app);
    bool loadAccount(TestAccountPtr account, Application& app);

    std::pair<TestAccountPtr, TestAccountPtr>
    pickAccountPair(uint32_t numAccounts, uint32_t offset, uint32_t ledgerNum,
                    uint64_t sourceAccountId);
    TestAccountPtr findAccount(uint64_t accountId, uint32_t ledgerNum);

    std::pair<TestAccountPtr, TransactionFramePtr>
    paymentTransaction(uint32_t numAccounts, uint32_t offset,
                       uint32_t ledgerNum, uint64_t sourceAccount,
                       uint32_t opCount,
                       std::optional<uint32_t> maxGeneratedFeeRate);
    std::pair<TestAccountPtr, TransactionFramePtr>
    pretendTransaction(uint32_t numAccounts, uint32_t offset,
                       uint32_t ledgerNum, uint64_t sourceAccount,
                       uint32_t opCount,
                       std::optional<uint32_t> maxGeneratedFeeRate);
    std::pair<LoadGenerator::TestAccountPtr, TransactionFramePtr>
    manageOfferTransaction(uint32_t ledgerNum, uint64_t accountId,
                           uint32_t opCount,
                           std::optional<uint32_t> maxGeneratedFeeRate);
    std::pair<LoadGenerator::TestAccountPtr, TransactionFramePtr>
    uploadWasmTransaction(uint32_t ledgerNum, uint64_t accountId,
                          uint32_t inclusionFee);
    std::pair<LoadGenerator::TestAccountPtr, TransactionFramePtr>
    createContractTransaction(uint32_t ledgerNum, uint64_t accountId,
                              uint32_t inclusionFee);
    std::pair<LoadGenerator::TestAccountPtr, TransactionFramePtr>
    invokeSorobanLoadTransaction(uint32_t ledgerNum, uint64_t accountId,
                                 GeneratedLoadConfig const& cfg);
    std::pair<LoadGenerator::TestAccountPtr, TransactionFramePtr>
    sorobanRandomWasmTransaction(uint32_t ledgerNum, uint64_t accountId,
                                 SorobanResources resources, size_t wasmSize,
                                 uint32_t inclusionFee);
    void maybeHandleFailedTx(TransactionFramePtr tx,
                             TestAccountPtr sourceAccount,
                             TransactionQueue::AddResult status,
                             TransactionResultCode code);
    std::pair<TestAccountPtr, TransactionFramePtr>
    creationTransaction(uint64_t startAccount, uint64_t numItems,
                        uint32_t ledgerNum);
    void logProgress(std::chrono::nanoseconds submitTimer, LoadGenMode mode,
                     uint32_t nAccounts, uint32_t nTxs, uint32_t txRate);

    uint32_t submitCreationTx(uint32_t nAccounts, uint32_t offset,
                              uint32_t ledgerNum);
    uint32_t submitSorobanPrepareInvokeTX(uint32_t nInstances,
                                          uint32_t ledgerNum,
                                          uint32_t inclusionFee);
    bool submitTx(GeneratedLoadConfig const& cfg,
                  std::function<std::pair<LoadGenerator::TestAccountPtr,
                                          TransactionFramePtr>()>
                      generateTx);
    void waitTillComplete(LoadGenMode mode);
    void waitTillCompleteWithoutChecks();

    void updateMinBalance();

    unsigned short chooseOpCount(Config const& cfg) const;

    void cleanupAccounts();
};
}
