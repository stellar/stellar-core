// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

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
    PAY,
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
    MIXED_CLASSIC_SOROBAN,
    // Submit pre-generated payment transactions from an XDR file
    PAY_PREGENERATED,
    // Overlay-only modes: pre-generated classic payments + a soroban
    // transaction type of choice, each with its own TPS. No on-ledger setup
    // is required; soroban contract keys are synthesized in memory. Apply
    // is skipped so nothing is actually executed.
    MIXED_PREGEN_SAC_PAYMENT,
    MIXED_PREGEN_OZ_TOKEN_TRANSFER,
    MIXED_PREGEN_SOROSWAP_SWAP
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

    // Config settings for the MIXED_PREGEN_* overlay-only modes.
    // Each stream has its own independent TPS. Setting `classicTxRate` to 0
    // yields a pure-soroban run; `sorobanTxRate` to 0 is equivalent to
    // PAY_PREGENERATED. `nTxs` is the combined stop target.
    struct MixPregenSorobanConfig
    {
        uint32_t classicTxRate = 0;
        uint32_t sorobanTxRate = 0;
    };

    void copySorobanNetworkConfigToUpgradeConfig(
        SorobanNetworkConfig const& baseConfig,
        SorobanNetworkConfig const& updatedConfig);

    static GeneratedLoadConfig createSorobanInvokeSetupLoad(uint32_t nAccounts,
                                                            uint32_t nInstances,
                                                            uint32_t txRate);

    static GeneratedLoadConfig createSorobanUpgradeSetupLoad();

    static GeneratedLoadConfig
    txLoad(LoadGenMode mode, uint32_t nAccounts, uint32_t nTxs, uint32_t txRate,
           uint32_t offset = 0, std::optional<uint32_t> maxFee = std::nullopt);

    static GeneratedLoadConfig
    pregeneratedTxLoad(uint32_t nAccounts, uint32_t nTxs, uint32_t txRate,
                       uint32_t offset, std::filesystem::path const& file);

    SorobanConfig& getMutSorobanConfig();
    SorobanConfig const& getSorobanConfig() const;
    SorobanUpgradeConfig& getMutSorobanUpgradeConfig();
    SorobanUpgradeConfig const& getSorobanUpgradeConfig() const;
    MixClassicSorobanConfig& getMutMixClassicSorobanConfig();
    MixClassicSorobanConfig const& getMixClassicSorobanConfig() const;
    MixPregenSorobanConfig& getMutMixPregenSorobanConfig();
    MixPregenSorobanConfig const& getMixPregenSorobanConfig() const;
    uint32_t getMinSorobanPercentSuccess() const;
    void setMinSorobanPercentSuccess(uint32_t minPercentSuccess);

    bool isSoroban() const;
    bool isSorobanSetup() const;
    bool isLoad() const;

    // True iff mode generates SOROBAN_INVOKE load
    bool modeInvokes() const;

    // True iff mode emits a classic pregen stream in parallel with a soroban
    // stream (MIXED_PREGEN_* modes).
    bool modeMixesPregen() const;

    // True iff mode generates SOROBAN_INVOKE_SETUP load
    bool modeSetsUpInvoke() const;

    // True iff mode generates SOROBAN_UPLOAD load
    bool modeUploads() const;

    bool isDone() const;
    bool areTxsRemaining() const;
    Json::Value getStatus() const;

    LoadGenMode mode = LoadGenMode::PAY;
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
    std::optional<uint32_t> maxGeneratedFeeRate;
    // When true, skips when they're not accepted by Herder due to low fee (due
    // to `TxQueueLimiter` limiting the operation count per ledger). Otherwise,
    // the load generation will fail after a couple of retries.
    bool skipLowFeeTxs = false;
    // Path to the pre-generated transactions file for PAY_PREGENERATED mode
    std::filesystem::path preloadedTransactionsFile;

  private:
    SorobanConfig sorobanConfig;
    SorobanUpgradeConfig sorobanUpgradeConfig;
    MixClassicSorobanConfig mixClassicSorobanConfig;
    MixPregenSorobanConfig mixPregenSorobanConfig;

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
    checkAccountSynced(Application& app);
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

    void cleanupAccounts(PerPhaseTransactionList const& perPhaseTxs);

  private:
    struct TxMetrics
    {
        medida::Meter& mNativePayment;
        medida::Meter& mSorobanUploadTxs;
        medida::Meter& mSorobanSetupInvokeTxs;
        medida::Meter& mSorobanSetupUpgradeTxs;
        medida::Meter& mSorobanInvokeTxs;
        medida::Meter& mSorobanCreateUpgradeTxs;
        medida::Meter& mTxnAttempted;
        medida::Meter& mTxnRejected;
        medida::Meter& mTxnBytes;
        medida::Meter& mNativePaymentBytes;

        TxMetrics(MetricsRegistry& m);
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
    TxSubmitStatus execute(TransactionFrameBasePtr txf, LoadGenMode mode,
                           TransactionResultCode& code);

    static uint32_t const STEP_MSECS;
    static uint32_t const TX_SUBMIT_MAX_TRIES;
    static uint32_t const TIMEOUT_NUM_LEDGERS;
    static uint32_t const COMPLETION_TIMEOUT_WITHOUT_CHECKS;
    static uint32_t const MIN_UNIQUE_ACCOUNT_MULTIPLIER;

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

    std::optional<XDRInputFileStream> mPreloadedTransactionsFile;
    uint32_t mCurrPreloadedTransaction = 0;

    // Get an account ID not currently in use.
    uint64_t getNextAvailableAccount(uint32_t ledgerNum);

    // Number of times `createContractTransaction` has been called. Used to
    // ensure unique preimages for all `SOROBAN_UPGRADE_SETUP` runs.
    uint32_t mNumCreateContractTransactionCalls = 0;

    medida::Timer& mStepTimer;
    medida::Meter& mStepMeter;
    mutable TxMetrics mTxMetrics;
    medida::Timer& mApplyTxTimer;
    medida::Timer& mApplyOpTimer;

    // Internal loadgen state gets reset after each run, but it is impossible to
    // regenerate contract instance keys for DB lookup. Due to this we maintain
    // a list of instances and the wasm entry which we use to rebuild
    // mContractInstances at the start of each SOROBAN_INVOKE run
    UnorderedSet<LedgerKey> mContractInstanceKeys = {};
    std::optional<LedgerKey> mCodeKey = std::nullopt;
    uint64_t mContactOverheadBytes = 0;

    // Maps account ID to it's contract instance, where each account has a
    // unique instance
    UnorderedMap<uint64_t, TxGenerator::ContractInstance> mContractInstances;

    // Synthetic (off-ledger) contract state for the MIXED_PREGEN_* overlay-only
    // modes. Lazily populated on first use of each mode; never touches the DB.
    std::optional<TxGenerator::ContractInstance> mSyntheticSACInstance;
    std::optional<TxGenerator::ContractInstance> mSyntheticTokenInstance;
    std::optional<TxGenerator::SoroswapState> mSyntheticSoroswapState;
    // Counter for generating unique SAC payment destination addresses.
    uint32_t mSyntheticSACDestCounter{0};
    // Round-robin counter for Soroswap pair selection + direction.
    uint32_t mSyntheticSoroswapSwapCounter{0};
    // Per-stream submission counters, tracked for every mode. For single-
    // stream modes one of them stays at 0, which makes the waitTillComplete
    // check trivially hold for that stream. For MIXED_PREGEN_* both are used.
    uint64_t mClassicSubmitted{0};
    uint64_t mSorobanSubmitted{0};
    // Per-stream applied-at-start snapshots captured in start(), so
    // waitTillComplete compares per-stream applied deltas against per-stream
    // submitted counters.
    uint32_t mClassicAppliedAtStart{0};
    uint32_t mSorobanAppliedAtStart{0};

    TxGenerator::TestAccountPtr mRoot;

    medida::Meter& mLoadgenComplete;
    medida::Meter& mLoadgenFail;

    // Counts of successful and failed soroban transactions prior to running
    // loadgen
    int64_t mPreLoadgenApplySorobanSuccess = 0;
    int64_t mPreLoadgenApplySorobanFailure = 0;

    bool mFailed{false};
    bool mStarted{false};

    uint32_t mWaitTillCompleteForLedgers{0};

    // Mode used for last mixed transaction in MIX_CLASSIC_SOROBAN mode
    LoadGenMode mLastMixedMode;

    void reset();
    void resetSorobanState();
    int64_t getTxPerStep(uint32_t txRate, std::chrono::seconds spikeInterval,
                         uint32_t spikeSize);
    // Variant that paces against a caller-provided submitted counter, used for
    // MIXED_PREGEN_* where classic and soroban streams have independent rates
    // and must not share mTotalSubmitted.
    int64_t getTxPerStep(uint32_t txRate, std::chrono::seconds spikeInterval,
                         uint32_t spikeSize, uint64_t submittedSoFar);

    // Schedule a callback to generateLoad() STEP_MSECS milliseconds from now.
    void scheduleLoadGeneration(GeneratedLoadConfig cfg);

    // Create a transaction in MIXED_CLASSIC_SOROBAN mode
    std::pair<TxGenerator::TestAccountPtr, TransactionFrameBaseConstPtr>
    createMixedClassicSorobanTransaction(
        uint32_t ledgerNum, uint64_t sourceAccountId,
        std::optional<uint32_t> classicByteCount,
        GeneratedLoadConfig const& cfg);

    // Build a synthetic-state soroban transaction for the requested mode
    // (one of MIXED_PREGEN_SAC_PAYMENT / OZ_TOKEN_TRANSFER / SOROSWAP_SWAP).
    // Lazily initializes the mSynthetic* state on first call.
    std::pair<TxGenerator::TestAccountPtr, TransactionFrameBaseConstPtr>
    createSyntheticSorobanTransaction(uint32_t ledgerNum,
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
                             TxSubmitStatus status, TransactionResultCode code);

    void logProgress(std::chrono::nanoseconds submitTimer,
                     GeneratedLoadConfig const& cfg) const;

    bool submitTx(GeneratedLoadConfig const& cfg,
                  std::function<std::pair<TxGenerator::TestAccountPtr,
                                          TransactionFrameBaseConstPtr>()>
                      generateTx);
    void waitTillComplete(GeneratedLoadConfig cfg);
    void waitTillCompleteWithoutChecks();

    std::optional<uint32_t> chooseByteCount(Config const& cfg) const;

    void start(GeneratedLoadConfig& cfg);

    // Indicate load generation run failed. Set `resetSoroban` to `true` to
    // reset soroban state.
    void emitFailure(bool resetSoroban);

    // Generate transaction by reading a pre-generated transaction from an XDR
    // file
    std::pair<TxGenerator::TestAccountPtr, TransactionFrameBaseConstPtr>
    readTransactionFromFile(GeneratedLoadConfig const& cfg);
};
}
