#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SecretKey.h"
#include "herder/Herder.h"
#include "main/Application.h"
#include "simulation/TxGenerator.h"
#include "test/TestAccount.h"
#include "test/TxTests.h"
#include "util/NonCopyable.h"
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
    MIXED_CLASSIC,
    // Deploy random Wasm blobs, for overlay/herder testing
    SOROBAN_UPLOAD,
    // Deploy contracts to be used by SOROBAN_INVOKE
    SOROBAN_INVOKE_SETUP,
    // Invoke and apply resource intensive TXs, must run SOROBAN_INVOKE_SETUP
    // first
    SOROBAN_INVOKE,
    // Setup contract instance for Soroban Network Config Upgrade
    SOROBAN_UPGRADE_SETUP,
    // Create upgrade entry
    SOROBAN_CREATE_UPGRADE,
    // Blend classic and soroban transactions. Mix of pay, upload, and invoke.
    MIXED_CLASSIC_SOROBAN
};

struct GeneratedLoadConfig
{
    // Config parameters for SOROBAN_INVOKE_SETUP, SOROBAN_INVOKE,
    // SOROBAN_UPGRADE_SETUP, SOROBAN_CREATE_UPGRADE, and MIXED_CLASSIC_SOROBAN
    struct SorobanConfig
    {
        uint32_t nInstances = 0;
        // For now, this value is automatically set to one. A future update will
        // enable multiple Wasm entries
        uint32_t nWasms = 0;
    };

    // Config settings for MIXED_CLASSIC_SOROBAN
    struct MixClassicSorobanConfig
    {
        // Weights determining the distribution of PAY, SOROBAN_UPLOAD, and
        // SOROBAN_INVOKE load
        double payWeight = 0;
        double sorobanUploadWeight = 0;
        double sorobanInvokeWeight = 0;
    };

    void
    copySorobanNetworkConfigToUpgradeConfig(SorobanNetworkConfig const& cfg);

    static GeneratedLoadConfig createAccountsLoad(uint32_t nAccounts,
                                                  uint32_t txRate);

    static GeneratedLoadConfig createSorobanInvokeSetupLoad(uint32_t nAccounts,
                                                            uint32_t nInstances,
                                                            uint32_t txRate);

    static GeneratedLoadConfig createSorobanUpgradeSetupLoad();

    static GeneratedLoadConfig
    txLoad(LoadGenMode mode, uint32_t nAccounts, uint32_t nTxs, uint32_t txRate,
           uint32_t offset = 0, std::optional<uint32_t> maxFee = std::nullopt);

    SorobanConfig& getMutSorobanConfig();
    SorobanConfig const& getSorobanConfig() const;
    SorobanUpgradeConfig& getMutSorobanUpgradeConfig();
    SorobanUpgradeConfig const& getSorobanUpgradeConfig() const;
    MixClassicSorobanConfig& getMutMixClassicSorobanConfig();
    MixClassicSorobanConfig const& getMixClassicSorobanConfig() const;
    uint32_t& getMutDexTxPercent();
    uint32_t const& getDexTxPercent() const;
    uint32_t getMinSorobanPercentSuccess() const;
    void setMinSorobanPercentSuccess(uint32_t minPercentSuccess);

    bool isCreate() const;
    bool isSoroban() const;
    bool isSorobanSetup() const;
    bool isLoad() const;

    // True iff mode generates SOROBAN_INVOKE load
    bool modeInvokes() const;

    // True iff mode generates SOROBAN_INVOKE_SETUP load
    bool modeSetsUpInvoke() const;

    // True iff mode generates SOROBAN_UPLOAD load
    bool modeUploads() const;

    bool isDone() const;
    bool areTxsRemaining() const;
    Json::Value getStatus() const;

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

  private:
    SorobanConfig sorobanConfig;
    SorobanUpgradeConfig sorobanUpgradeConfig;
    MixClassicSorobanConfig mixClassicSorobanConfig;

    // Percentage (from 0 to 100) of DEX transactions
    uint32_t dexTxPercent = 0;

    // Minimum percentage of successful soroban transactions for run to be
    // considered successful.
    uint32_t mMinSorobanPercentSuccess = 0;
};

class LoadGenerator
{
  public:
    LoadGenerator(Application& app);

    static LoadGenMode getMode(std::string const& mode);

    // Returns true if loadgen has submitted all required TXs
    bool isDone(GeneratedLoadConfig const& cfg) const;

    // Returns true if loadgen can continue and does not need to wait for Wasm
    // application
    bool checkSorobanWasmSetup(GeneratedLoadConfig const& cfg);

    // Returns true if at least `cfg.minPercentSuccess`% of the SOROBAN_INVOKE
    // load that made it into a block was successful. Always returns true for
    // modes that do not generate SOROBAN_INVOKE load.
    bool checkMinimumSorobanSuccess(GeneratedLoadConfig const& cfg);

    // Generate one "step" worth of load (assuming 1 step per STEP_MSECS) at a
    // given target number of accounts and txs, a given target tx/s rate, and
    // according to the other parameters provided in configuration.
    // If work remains after the current step, calls scheduleLoadGeneration()
    // with the remainder.
    void generateLoad(GeneratedLoadConfig cfg);

    ConfigUpgradeSetKey
    getConfigUpgradeSetKey(SorobanUpgradeConfig const& upgradeCfg) const;

    // Verify cached accounts are properly reflected in the database
    // return any accounts that are inconsistent.
    std::vector<TxGenerator::TestAccountPtr>
    checkAccountSynced(Application& app, bool isCreate);
    std::vector<LedgerKey>
    checkSorobanStateSynced(Application& app, GeneratedLoadConfig const& cfg);

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

    uint64_t
    getContactOverheadBytesForTesting() const
    {
        return mContactOverheadBytes;
    }

    void stop();

  private:
    struct TxMetrics
    {
        medida::Meter& mAccountCreated;
        medida::Meter& mNativePayment;
        medida::Meter& mManageOfferOps;
        medida::Meter& mPretendOps;
        medida::Meter& mSorobanUploadTxs;
        medida::Meter& mSorobanSetupInvokeTxs;
        medida::Meter& mSorobanSetupUpgradeTxs;
        medida::Meter& mSorobanInvokeTxs;
        medida::Meter& mSorobanCreateUpgradeTxs;
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
    TransactionQueue::AddResultCode execute(TransactionFrameBasePtr txf,
                                            LoadGenMode mode,
                                            TransactionResultCode& code);

    static const uint32_t STEP_MSECS;
    static const uint32_t TX_SUBMIT_MAX_TRIES;
    static const uint32_t TIMEOUT_NUM_LEDGERS;
    static const uint32_t COMPLETION_TIMEOUT_WITHOUT_CHECKS;
    static const uint32_t MIN_UNIQUE_ACCOUNT_MULTIPLIER;

    TxGenerator mTxGenerator;
    Application& mApp;

    std::unique_ptr<VirtualTimer> mLoadTimer;
    uint64_t mLastSecond;
    int64_t mTotalSubmitted;
    // Set when load generation actually begins
    std::unique_ptr<VirtualClock::time_point> mStartTime;

    // Track account IDs that are currently being referenced by the transaction
    // queue (to avoid source account collisions during tx submission)
    std::unordered_set<uint64_t> mAccountsInUse;
    std::unordered_set<uint64_t> mAccountsAvailable;

    // Get an account ID not currently in use.
    uint64_t getNextAvailableAccount(uint32_t ledgerNum);

    // Number of times `createContractTransaction` has been called. Used to
    // ensure unique preimages for all `SOROBAN_UPGRADE_SETUP` runs.
    uint32_t mNumCreateContractTransactionCalls = 0;

    // For account creation only: allocate a few accounts for creation purposes
    // (with sufficient balance to create new accounts) to avoid source account
    // collisions.
    std::unordered_map<uint64_t, TxGenerator::TestAccountPtr>
        mCreationSourceAccounts;

    medida::Timer& mStepTimer;
    medida::Meter& mStepMeter;
    mutable TxMetrics mTxMetrics;
    medida::Timer& mApplyTxTimer;
    medida::Timer& mApplyOpTimer;

    // Internal loadgen state gets reset after each run, but it is impossible to
    // regenerate contract instance keys for DB lookup. Due to this we maintain
    // a static list of instances and the wasm entry which we use to rebuild
    // mContractInstances at the start of each SOROBAN_INVOKE run
    inline static UnorderedSet<LedgerKey> mContractInstanceKeys = {};
    inline static std::optional<LedgerKey> mCodeKey = std::nullopt;
    inline static uint64_t mContactOverheadBytes = 0;

    // Maps account ID to it's contract instance, where each account has a
    // unique instance
    UnorderedMap<uint64_t, TxGenerator::ContractInstance> mContractInstances;

    TxGenerator::TestAccountPtr mRoot;

    medida::Meter& mLoadgenComplete;
    medida::Meter& mLoadgenFail;

    // Counts of successful and failed soroban transactions prior to running
    // loadgen
    int64_t mPreLoadgenApplySorobanSuccess = 0;
    int64_t mPreLoadgenApplySorobanFailure = 0;

    bool mFailed{false};
    bool mStarted{false};
    bool mInitialAccountsCreated{false};

    uint32_t mWaitTillCompleteForLedgers{0};
    uint32_t mSorobanWasmWaitTillLedgers{0};

    // Mode used for last mixed transaction in MIX_CLASSIC_SOROBAN mode
    LoadGenMode mLastMixedMode;

    void createRootAccount();

    void reset();
    void resetSorobanState();
    int64_t getTxPerStep(uint32_t txRate, std::chrono::seconds spikeInterval,
                         uint32_t spikeSize);

    // Schedule a callback to generateLoad() STEP_MSECS milliseconds from now.
    void scheduleLoadGeneration(GeneratedLoadConfig cfg);

    // Create a transaction in MIXED_CLASSIC_SOROBAN mode
    std::pair<TxGenerator::TestAccountPtr, TransactionFrameBaseConstPtr>
    createMixedClassicSorobanTransaction(uint32_t ledgerNum,
                                         uint64_t sourceAccountId,
                                         GeneratedLoadConfig const& cfg);

    std::pair<TxGenerator::TestAccountPtr, TransactionFrameBaseConstPtr>
    createUploadWasmTransaction(GeneratedLoadConfig const& cfg,
                                uint32_t ledgerNum, uint64_t sourceAccountId);

    std::pair<TxGenerator::TestAccountPtr, TransactionFrameBaseConstPtr>
    createInstanceTransaction(GeneratedLoadConfig const& cfg,
                              uint32_t ledgerNum, uint64_t sourceAccountId);

    // Samples a random wasm size from the `LOADGEN_WASM_BYTES_FOR_TESTING`
    // distribution. Returns a pair containing the appropriate resources for a
    // wasm of that size as well as the size itself.
    std::pair<SorobanResources, uint32_t> sorobanRandomUploadResources();
    void maybeHandleFailedTx(TransactionFrameBaseConstPtr tx,
                             TxGenerator::TestAccountPtr sourceAccount,
                             TransactionQueue::AddResultCode status,
                             TransactionResultCode code);
    std::pair<TxGenerator::TestAccountPtr, TransactionFrameBaseConstPtr>
    creationTransaction(uint64_t startAccount, uint64_t numItems,
                        uint32_t ledgerNum);
    void logProgress(std::chrono::nanoseconds submitTimer,
                     GeneratedLoadConfig const& cfg) const;

    uint32_t submitCreationTx(uint32_t nAccounts, uint32_t offset,
                              uint32_t ledgerNum);
    bool submitTx(GeneratedLoadConfig const& cfg,
                  std::function<std::pair<TxGenerator::TestAccountPtr,
                                          TransactionFrameBaseConstPtr>()>
                      generateTx);
    void waitTillComplete(GeneratedLoadConfig cfg);
    void waitTillCompleteWithoutChecks();

    unsigned short chooseOpCount(Config const& cfg) const;

    void cleanupAccounts();

    void start(GeneratedLoadConfig& cfg);

    // Indicate load generation run failed. Set `resetSoroban` to `true` to
    // reset soroban state.
    void emitFailure(bool resetSoroban);
};
}
