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
    // Deploy random WASM blobs, for overlay testing
    SOROBAN_WASM,
    // Invoke and apply resource intensive TXs
    SOROBAN_INVOKE
};

// Soroban load gen occurs in 4 steps:
enum class SorobanPhase
{
    UPLOAD,  // Upload WASM blobs
    CREATE,  // Create contract instances
    STORAGE, // Create storage entries
    INVOKE   // Invoke CPU heavy functions
};

struct GeneratedLoadConfig
{
    static GeneratedLoadConfig createAccountsLoad(uint32_t nAccounts,
                                                  uint32_t txRate);

    static GeneratedLoadConfig
    txLoad(LoadGenMode mode, uint32_t nAccounts, uint32_t nTxs, uint32_t txRate,
           uint32_t offset = 0, std::optional<uint32_t> maxFee = std::nullopt);

    LoadGenMode mode = LoadGenMode::CREATE;
    SorobanPhase sorobanPhase = SorobanPhase::UPLOAD;
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
    // Number and size of ContractData entries that will loaded on each
    // invocation
    uint32_t nDataEntries = 0;
    uint32_t bytesPerDataEntry = 0;

    // If true, soroban metadata will be preserved on reset for testing purposes
    bool sorobanNoResetForTesting = false;
};

class LoadGenerator
{
  public:
    using TestAccountPtr = std::shared_ptr<TestAccount>;
    LoadGenerator(Application& app);

    static LoadGenMode getMode(std::string const& mode);

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

    struct ContractInstance
    {
        // [wasm, instance, data keys...]
        xdr::xvector<LedgerKey> readOnlyKeys;
        SCAddress contractID;
    };

    std::unordered_map<uint64_t, ContractInstance>
    getContractInstancesForTesting() const
    {
        return mCompleteContractInstances;
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

    typedef std::function<
        std::pair<LoadGenerator::TestAccountPtr, TransactionFramePtr>()>
        LoadGenFunc;

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

    static const uint32_t STEP_MSECS;
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
    std::optional<uint64_t>
    getNextAvailableAccountForSoroban(GeneratedLoadConfig const& cfg);

    // For account creation only: allocate a few accounts for creation purposes
    // (with sufficient balance to create new accounts) to avoid source account
    // collisions.
    std::unordered_map<uint64_t, TestAccountPtr> mCreationSourceAccounts;

    std::optional<LedgerKey> mCodeKey;

    // Maps entries that are being created by in flight TXs to the source
    // account ID originating the TX
    std::unordered_map<uint64_t, std::vector<LedgerKey>> mPendingEntries;

    // Maps account ID to it's contract instance, where each account has a
    // unique instance
    std::unordered_map<uint64_t, ContractInstance> mIncompleteContractInstances;

    std::unordered_map<uint64_t, ContractInstance> mCompleteContractInstances;

    medida::Meter& mLoadgenComplete;
    medida::Meter& mLoadgenFail;

    bool mFailed{false};
    bool mStarted{false};
    bool mInitialAccountsCreated{false};

    uint32_t mWaitTillCompleteForLedgers{0};

    void reset(bool sorobanNoReset);
    void createRootAccount();
    int64_t getTxPerStep(uint32_t txRate, std::chrono::seconds spikeInterval,
                         uint32_t spikeSize);
    void checkPendingTxs(GeneratedLoadConfig const& cfg);

    // Schedule a callback to generateLoad() STEP_MSECS milliseconds from now.
    void scheduleLoadGeneration(GeneratedLoadConfig cfg);

    std::vector<Operation> createAccounts(uint64_t i, uint64_t batchSize,
                                          uint32_t ledgerNum,
                                          bool initialAccounts);
    bool loadAccount(TestAccount& account, Application& app);
    bool loadAccount(TestAccountPtr account, Application& app);

    std::pair<TestAccountPtr, TestAccountPtr>
    pickAccountPair(uint32_t numAccounts, uint32_t offset, uint32_t ledgerNum,
                    uint64_t sourceAccountId);
    TestAccountPtr findAccount(uint64_t accountId, uint32_t ledgerNum);

    // Sets generateTx to correct soroban loadgen function. Returns false if TX
    // could not be created due to pending TXs, true otherwise.
    bool sorobanInvokeLoadGenStep(GeneratedLoadConfig& cfg, uint32_t ledgerNum,
                                  LoadGenFunc& generateTx);

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
    invokeStorageTransaction(uint32_t ledgerNum, uint64_t accountId,
                             uint32_t inclusionFee);
    std::pair<LoadGenerator::TestAccountPtr, TransactionFramePtr>
    invokeSorobanLoadTransaction(uint32_t ledgerNum, uint64_t accountId,
                                 uint32_t inclusionFee);
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
    bool submitTx(GeneratedLoadConfig const& cfg,
                  std::function<std::pair<LoadGenerator::TestAccountPtr,
                                          TransactionFramePtr>()>
                      generateTx);
    void waitTillComplete(bool isCreate, bool sorobanNoReset);
    void waitTillCompleteWithoutChecks(bool sorobanNoReset);

    void updateMinBalance();

    unsigned short chooseOpCount(Config const& cfg) const;

    void cleanupAccounts();
};
}
