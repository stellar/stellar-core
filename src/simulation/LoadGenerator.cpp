// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "simulation/LoadGenerator.h"
#include "database/Database.h"
#include "herder/Herder.h"
#include "ledger/LedgerManager.h"
#include "main/Config.h"
#include "overlay/OverlayManager.h"
#include "test/TestAccount.h"
#include "test/TxTests.h"
#include "transactions/MutableTransactionResult.h"
#include "transactions/TransactionBridge.h"
#include "transactions/TransactionSQL.h"
#include "transactions/TransactionUtils.h"
#include "transactions/test/SorobanTxTestUtils.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/Timer.h"
#include "util/XDRCereal.h"
#include "util/XDRStream.h"
#include "util/numeric.h"
#include "util/types.h"

#include "xdrpp/marshal.h"

#include "medida/meter.h"
#include "medida/metrics_registry.h"

#include "ledger/test/LedgerTestUtils.h"
#include <Tracy.hpp>
#include <cmath>
#include <crypto/SHA.h>
#include <fmt/format.h>
#include <iomanip>
#include <set>

namespace stellar
{

using namespace std;
using namespace txtest;

namespace
{
// Default distribution settings, largely based on averages seen on testnet
// Sample from a discrete distribution of `values` with weights `weights`.
// Returns `defaultValue` if `values` is empty.
template <typename T>
T
sampleDiscrete(std::vector<T> const& values,
               std::vector<uint32_t> const& weights, T defaultValue)
{
    if (values.empty())
    {
        return defaultValue;
    }

    std::discrete_distribution<uint32_t> distribution(weights.begin(),
                                                      weights.end());
    return values.at(distribution(getGlobalRandomEngine()));
}
} // namespace

// Units of load are scheduled at 100ms intervals.
const uint32_t LoadGenerator::STEP_MSECS = 100;

// If submission fails with txBAD_SEQ, attempt refreshing the account or
// re-submitting a new payment
const uint32_t LoadGenerator::TX_SUBMIT_MAX_TRIES = 10;

// After successfully submitting desired load, wait a bit to let it get into the
// ledger.
const uint32_t LoadGenerator::TIMEOUT_NUM_LEDGERS = 20;

// After successfully submitting desired load, wait for this many ledgers
// without checking for account consistency.
const uint32_t LoadGenerator::COMPLETION_TIMEOUT_WITHOUT_CHECKS = 4;

// Minimum unique account multiplier. This is used to calculate the minimum
// number of accounts needed to sustain desired tx/s rate (this provides a
// buffer in case loadgen is unstable and needs more accounts)
const uint32_t LoadGenerator::MIN_UNIQUE_ACCOUNT_MULTIPLIER = 3;

uint32_t
getTxCount(Application& app, bool isSoroban)
{
    if (isSoroban)
    {
        return app.getMetrics()
            .NewCounter({"herder", "pending-soroban-txs", "self-count"})
            .count();
    }
    else
    {
        return app.getMetrics()
            .NewCounter({"herder", "pending-txs", "self-count"})
            .count();
    }
}

LoadGenerator::LoadGenerator(Application& app)
    : mTxGenerator(app)
    , mApp(app)
    , mLastSecond(0)
    , mTotalSubmitted(0)
    , mStepTimer(mApp.getMetrics().NewTimer({"loadgen", "step", "submit"}))
    , mStepMeter(
          mApp.getMetrics().NewMeter({"loadgen", "step", "count"}, "step"))
    , mTxMetrics(app.getMetrics())
    , mApplyTxTimer(
          mApp.getMetrics().NewTimer({"ledger", "transaction", "apply"}))
    , mApplyOpTimer(
          mApp.getMetrics().NewTimer({"ledger", "operation", "apply"}))
    , mRoot(app.getRoot())
    , mLoadgenComplete(
          mApp.getMetrics().NewMeter({"loadgen", "run", "complete"}, "run"))
    , mLoadgenFail(
          mApp.getMetrics().NewMeter({"loadgen", "run", "failed"}, "run"))
{
}

LoadGenMode
LoadGenerator::getMode(std::string const& mode)
{
    if (mode == "pay")
    {
        return LoadGenMode::PAY;
    }
    else if (mode == "mixed_classic")
    {
        return LoadGenMode::MIXED_CLASSIC;
    }
    else if (mode == "soroban_upload")
    {
        return LoadGenMode::SOROBAN_UPLOAD;
    }
    else if (mode == "soroban_invoke_setup")
    {
        return LoadGenMode::SOROBAN_INVOKE_SETUP;
    }
    else if (mode == "soroban_invoke")
    {
        return LoadGenMode::SOROBAN_INVOKE;
    }
    else if (mode == "upgrade_setup")
    {
        return LoadGenMode::SOROBAN_UPGRADE_SETUP;
    }
    else if (mode == "create_upgrade")
    {
        return LoadGenMode::SOROBAN_CREATE_UPGRADE;
    }
    else if (mode == "mixed_classic_soroban")
    {
        return LoadGenMode::MIXED_CLASSIC_SOROBAN;
    }
    else if (mode == "pay_pregenerated")
    {
        return LoadGenMode::PAY_PREGENERATED;
    }
    else if (mode == "soroban_invoke_apply_load")
    {
        return LoadGenMode::SOROBAN_INVOKE_APPLY_LOAD;
    }
    else
    {
        throw std::runtime_error(
            fmt::format(FMT_STRING("Unknown loadgen mode: {}"), mode));
    }
}

uint32_t
LoadGenerator::chooseByteCount(Config const& cfg) const
{
    return sampleDiscrete(cfg.LOADGEN_BYTE_COUNT_FOR_TESTING,
                          cfg.LOADGEN_BYTE_COUNT_DISTRIBUTION_FOR_TESTING, 0u);
}

int64_t
LoadGenerator::getTxPerStep(uint32_t txRate, std::chrono::seconds spikeInterval,
                            uint32_t spikeSize)
{
    if (!mStartTime)
    {
        throw std::runtime_error("Load generation start time must be set");
    }

    mStepMeter.Mark();

    auto now = mApp.getClock().now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - *mStartTime);
    auto txs =
        bigDivideOrThrow(elapsed.count(), txRate, 1000, Rounding::ROUND_DOWN);
    if (spikeInterval.count() > 0)
    {
        txs += bigDivideOrThrow(
                   std::chrono::duration_cast<std::chrono::seconds>(elapsed)
                       .count(),
                   1, spikeInterval.count(), Rounding::ROUND_DOWN) *
               spikeSize;
    }

    if (txs <= mTotalSubmitted)
    {
        return 0;
    }

    return txs - mTotalSubmitted;
}

void
LoadGenerator::cleanupAccounts()
{
    ZoneScoped;

    // "Free" any accounts that aren't used by the tx queue anymore
    for (auto it = mAccountsInUse.begin(); it != mAccountsInUse.end();)
    {
        auto const& accounts = mTxGenerator.getAccounts();
        auto accIt = accounts.find(*it);
        releaseAssert(accIt != accounts.end());
        if (!mApp.getHerder().sourceAccountPending(
                accIt->second->getPublicKey()))
        {
            mAccountsAvailable.insert(*it);
            it = mAccountsInUse.erase(it);
        }
        else
        {
            it++;
        }
    }
}

// Reset everything except Soroban persistent state
// Do not reset pregenerated transaction file position either
void
LoadGenerator::reset()
{
    mTxGenerator.reset();
    mAccountsInUse.clear();
    mAccountsAvailable.clear();

    mContractInstances.clear();
    mLoadTimer.reset();
    mStartTime.reset();
    mTotalSubmitted = 0;
    mWaitTillCompleteForLedgers = 0;
    mFailed = false;
    mStarted = false;

    mPreLoadgenApplySorobanSuccess = 0;
    mPreLoadgenApplySorobanFailure = 0;
    mTransactionsAppliedAtTheStart = 0;
}

// Reset Soroban persistent state
void
LoadGenerator::resetSorobanState()
{
    mContractInstanceKeys.clear();
    mCodeKey.reset();
    mContactOverheadBytes = 0;
}

void
LoadGenerator::stop()
{
    ZoneScoped;
    if (mStarted)
    {
        // Some residual transactions might still be pending in consensus, but
        // that should be harmless.
        if (mLoadTimer)
        {
            mLoadTimer->cancel();
        }
        mLoadgenFail.Mark();
        reset();
    }
}

void
LoadGenerator::start(GeneratedLoadConfig& cfg)
{
    if (mStarted)
    {
        return;
    }

    mTransactionsAppliedAtTheStart = getTxCount(mApp, cfg.isSoroban());
    if (cfg.mode == LoadGenMode::SOROBAN_INVOKE_APPLY_LOAD &&
        !mApp.getRunInOverlayOnlyMode())
    {
        reset();
        throw std::runtime_error(
            "Can only run SOROBAN_INVOKE_APPLY_LOAD in overlay only mode");
    }
    if (cfg.txRate == 0)
    {
        cfg.txRate = 1;
    }

    // Setup config for soroban modes
    if (cfg.isSoroban() && cfg.mode != LoadGenMode::SOROBAN_UPLOAD)
    {
        auto& sorobanLoadCfg = cfg.getMutSorobanConfig();
        sorobanLoadCfg.nWasms = 1;

        if (cfg.mode == LoadGenMode::SOROBAN_UPGRADE_SETUP)
        {
            // Only deploy single upgrade contract instance
            sorobanLoadCfg.nInstances = 1;
        }

        if (cfg.mode == LoadGenMode::SOROBAN_CREATE_UPGRADE)
        {
            // Submit a single upgrade TX
            sorobanLoadCfg.nInstances = 1;
            cfg.nTxs = 1;
        }

        if (cfg.isSorobanSetup())
        {
            resetSorobanState();

            // For first round of txs, we need to deploy the wasms.
            // waitTillFinished will set nTxs for instances once wasms have been
            // verified
            cfg.nTxs = sorobanLoadCfg.nWasms;

            // Must include all TXs
            cfg.skipLowFeeTxs = false;

            // No spikes during setup
            cfg.spikeInterval = std::chrono::seconds(0);
            cfg.spikeSize = 0;
        }

        if (cfg.modeSetsUpInvoke() || cfg.modeInvokes())
        {
            // Default instances to 1
            if (sorobanLoadCfg.nInstances == 0)
            {
                sorobanLoadCfg.nInstances = 1;
            }
        }
    }

    if (cfg.mode != LoadGenMode::PAY_PREGENERATED)
    {
        // For upgrade modes, use root account (represented by special ID)
        uint32_t accounts = cfg.nAccounts;
        if (cfg.mode == LoadGenMode::SOROBAN_UPGRADE_SETUP ||
            cfg.mode == LoadGenMode::SOROBAN_CREATE_UPGRADE)
        {
            mAccountsAvailable.insert(TxGenerator::ROOT_ACCOUNT_ID);
            if (accounts)
            {
                accounts--;
            }
        }

        // Mark all accounts "available" as source accounts
        for (auto i = 0u; i < accounts; i++)
        {
            mAccountsAvailable.insert(i + cfg.offset);
        }

        if (cfg.modeInvokes())
        {
            auto const& sorobanLoadCfg = cfg.getSorobanConfig();
            if (!mCodeKey)
            {
                // start is incomplete, so reset to avoid leaving the
                // LoadGenerator in an invalid state.
                reset();
                throw std::runtime_error(
                    "Before running MODE::SOROBAN_INVOKE, please run "
                    "MODE::SOROBAN_INVOKE_SETUP to set up your contract "
                    "first.");
            }
            releaseAssert(mContractInstances.empty());
            releaseAssert(mAccountsAvailable.size() >= cfg.nAccounts);
            releaseAssert(mContractInstanceKeys.size() >=
                          sorobanLoadCfg.nInstances);
            releaseAssert(cfg.nAccounts >= sorobanLoadCfg.nInstances);

            // assign a contract instance to each accountID
            auto accountIter = mAccountsAvailable.begin();
            for (size_t i = 0; i < cfg.nAccounts; ++i)
            {
                auto instanceKeyIter = mContractInstanceKeys.begin();
                std::advance(instanceKeyIter, i % sorobanLoadCfg.nInstances);

                TxGenerator::ContractInstance instance;
                instance.readOnlyKeys.emplace_back(*mCodeKey);
                instance.readOnlyKeys.emplace_back(*instanceKeyIter);
                instance.contractID = instanceKeyIter->contractData().contract;
                mContractInstances.emplace(*accountIter, instance);
                ++accountIter;
            }
        }
    }

    releaseAssert(!mLoadTimer);
    mLoadTimer = std::make_unique<VirtualTimer>(mApp.getClock());

    releaseAssert(!mStartTime);
    mStartTime =
        std::make_unique<VirtualClock::time_point>(mApp.getClock().now());

    releaseAssert(mPreLoadgenApplySorobanSuccess == 0);
    releaseAssert(mPreLoadgenApplySorobanFailure == 0);
    mPreLoadgenApplySorobanSuccess =
        mTxGenerator.getApplySorobanSuccess().count();
    mPreLoadgenApplySorobanFailure =
        mTxGenerator.getApplySorobanFailure().count();

    if (cfg.mode == LoadGenMode::PAY_PREGENERATED)
    {
        if (!mPreloadedTransactionsFile)
        {
            mPreloadedTransactionsFile = XDRInputFileStream();
            mPreloadedTransactionsFile->open(cfg.preloadedTransactionsFile);
        }

        // Preload all accounts
        for (auto i = 0u; i < cfg.nAccounts; i++)
        {
            auto actualId = i + cfg.offset;
            mTxGenerator.addAccount(
                actualId, std::make_shared<TestAccount>(
                              mApp,
                              txtest::getAccount("TestAccount-" +
                                                 std::to_string(actualId)),
                              0));
        }
    }
    mStarted = true;
}

ConfigUpgradeSetKey
LoadGenerator::getConfigUpgradeSetKey(
    SorobanUpgradeConfig const& upgradeCfg) const
{
    auto testingKeys = getContractInstanceKeysForTesting();
    releaseAssert(testingKeys.size() == 1);
    auto contractId = testingKeys.begin()->contractData().contract.contractId();

    return mTxGenerator.getConfigUpgradeSetKey(upgradeCfg, contractId);
}

// Schedule a callback to generateLoad() STEP_MSECS milliseconds from now.
void
LoadGenerator::scheduleLoadGeneration(GeneratedLoadConfig cfg)
{
    std::optional<std::string> errorMsg;
    // If previously scheduled step of load did not succeed, fail this loadgen
    // run.
    if (mFailed)
    {
        errorMsg = "Load generation failed, ensure correct "
                   "number parameters are set and accounts are "
                   "created, or retry with smaller tx rate.";
    }

    // During load submission, we must have enough unique source accounts (with
    // a buffer) to accommodate the desired tx rate.
    auto closeTimeSeconds = std::chrono::duration_cast<std::chrono::seconds>(
        mApp.getLedgerManager().getExpectedLedgerCloseTime());
    if (cfg.nTxs > cfg.nAccounts && (cfg.txRate * closeTimeSeconds.count()) *
                                            MIN_UNIQUE_ACCOUNT_MULTIPLIER >
                                        cfg.nAccounts)
    {
        errorMsg = fmt::format(
            "Tx rate is too high, there are not enough unique accounts. Make "
            "sure there are at least {}x "
            "unique accounts than desired number of transactions per ledger.",
            MIN_UNIQUE_ACCOUNT_MULTIPLIER);
    }

    if (cfg.isSoroban() &&
        protocolVersionIsBefore(mApp.getLedgerManager()
                                    .getLastClosedLedgerHeader()
                                    .header.ledgerVersion,
                                SOROBAN_PROTOCOL_VERSION))
    {
        errorMsg = "Soroban modes require protocol version 20 or higher";
    }

    if (cfg.modeInvokes())
    {
        auto const& sorobanLoadCfg = cfg.getSorobanConfig();
        releaseAssertOrThrow(sorobanLoadCfg.nInstances != 0);
        if (mContractInstanceKeys.size() < sorobanLoadCfg.nInstances ||
            !mCodeKey)
        {
            errorMsg = "must run SOROBAN_INVOKE_SETUP with at least nInstances";
        }
        else if (cfg.nAccounts < sorobanLoadCfg.nInstances)
        {
            errorMsg = "must have more accounts than instances";
        }
    }

    if (cfg.mode == LoadGenMode::SOROBAN_CREATE_UPGRADE)
    {
        if (mContractInstanceKeys.size() != 1 || !mCodeKey)
        {
            errorMsg = "must run SOROBAN_UPGRADE_SETUP";
        }
    }

    if (cfg.mode == LoadGenMode::PAY_PREGENERATED)
    {
        if (mApp.getConfig().GENESIS_TEST_ACCOUNT_COUNT == 0)
        {
            errorMsg = "PAY_PREGENERATED mode requires non-zero "
                       "GENESIS_TEST_ACCOUNT_COUNT";
        }
        else if (cfg.preloadedTransactionsFile.empty())
        {
            errorMsg =
                "PAY_PREGENERATED mode requires preloadedTransactionsFile";
        }
    }

    if (errorMsg)
    {
        CLOG_ERROR(LoadGen, "{}", *errorMsg);
        mLoadgenFail.Mark();
        reset();
        if (cfg.isSorobanSetup())
        {
            resetSorobanState();
        }

        return;
    }

    if (mApp.getState() == Application::APP_SYNCED_STATE)
    {
        mLoadTimer->expires_from_now(std::chrono::milliseconds(STEP_MSECS));
        mLoadTimer->async_wait([this, cfg]() { this->generateLoad(cfg); },
                               &VirtualTimer::onFailureNoop);
    }
    else
    {
        CLOG_WARNING(
            LoadGen,
            "Application is not in sync, load generation inhibited. State {}",
            mApp.getStateHuman());
        mLoadTimer->expires_from_now(std::chrono::seconds(10));
        mLoadTimer->async_wait(
            [this, cfg]() { this->scheduleLoadGeneration(cfg); },
            &VirtualTimer::onFailureNoop);
    }
}

bool
GeneratedLoadConfig::isDone() const
{
    return (isLoad() && nTxs == 0) ||
           (isSorobanSetup() && getSorobanConfig().nInstances == 0);
}

bool
GeneratedLoadConfig::areTxsRemaining() const
{
    return nTxs != 0;
}

Json::Value
GeneratedLoadConfig::getStatus() const
{
    Json::Value ret;
    std::string modeStr;
    switch (mode)
    {
    case LoadGenMode::PAY:
        modeStr = "pay";
        break;
    case LoadGenMode::MIXED_CLASSIC:
        modeStr = "mixed_classic";
        break;
    case LoadGenMode::SOROBAN_UPLOAD:
        modeStr = "soroban_upload";
        break;
    case LoadGenMode::SOROBAN_INVOKE_SETUP:
        modeStr = "soroban_invoke_setup";
        break;
    case LoadGenMode::SOROBAN_INVOKE:
        modeStr = "soroban_invoke";
        break;
    case LoadGenMode::SOROBAN_UPGRADE_SETUP:
        modeStr = "upgrade_setup";
        break;
    case LoadGenMode::SOROBAN_CREATE_UPGRADE:
        modeStr = "create_upgrade";
        break;
    case LoadGenMode::MIXED_CLASSIC_SOROBAN:
        modeStr = "mixed_classic_soroban";
        break;
    case LoadGenMode::PAY_PREGENERATED:
        modeStr = "pay_pregenerated";
        break;
    case LoadGenMode::SOROBAN_INVOKE_APPLY_LOAD:
        modeStr = "SOROBAN_INVOKE_APPLY_LOAD";
        break;
    }

    ret["mode"] = modeStr;

    if (isSorobanSetup())
    {
        ret["wasms_remaining"] = getSorobanConfig().nWasms;
        ret["instances_remaining"] = getSorobanConfig().nInstances;
    }
    else
    {
        ret["txs_remaining"] = nTxs;
    }

    ret["tx_rate"] = std::to_string(txRate) + " tx/s";
    if (mode == LoadGenMode::MIXED_CLASSIC)
    {
        ret["dex_tx_percent"] = std::to_string(getDexTxPercent()) + "%";
    }
    else if (modeInvokes())
    {
        ret["instances"] = getSorobanConfig().nInstances;
        ret["wasms"] = getSorobanConfig().nWasms;
    }

    if (mode == LoadGenMode::MIXED_CLASSIC_SOROBAN)
    {
        auto const& blendCfg = getMixClassicSorobanConfig();
        ret["pay_weight"] = blendCfg.payWeight;
        ret["soroban_upload_weight"] = blendCfg.sorobanUploadWeight;
        ret["soroban_invoke_weight"] = blendCfg.sorobanInvokeWeight;
    }

    if (isSoroban())
    {
        ret["min_soroban_percent_success"] = mMinSorobanPercentSuccess;
    }

    return ret;
}

// Generate one "step" worth of load (assuming 1 step per STEP_MSECS) at a
// given target number of accounts and txs, and a given target tx/s rate.
// If work remains after the current step, call scheduleLoadGeneration()
// with the remainder.
void
LoadGenerator::generateLoad(GeneratedLoadConfig cfg)
{
    ZoneScoped;

    start(cfg);

    // Finish if no more txs need to be created.
    if (!cfg.areTxsRemaining())
    {
        // Done submitting the load, now ensure it propagates to the DB.
        if (cfg.skipLowFeeTxs)
        {
            // skipLowFeeTxs allows triggering tx queue limiter, which
            // makes it hard to track the final seq nums. Hence just
            // wait unconditionally.
            waitTillCompleteWithoutChecks();
        }
        else
        {
            waitTillComplete(cfg);
        }
        return;
    }

    auto txPerStep = getTxPerStep(cfg.txRate, cfg.spikeInterval, cfg.spikeSize);
    auto submitScope = mStepTimer.TimeScope();

    uint64_t now = mApp.timeNow();
    // Cleaning up accounts every second, so we don't call potentially expensive
    // cleanup function too often
    if (now != mLastSecond && cfg.mode != LoadGenMode::PAY_PREGENERATED)
    {
        cleanupAccounts();
    }

    uint32_t ledgerNum = mApp.getLedgerManager().getLastClosedLedgerNum() + 1;
    uint32_t count = 0;
    for (int64_t i = 0; i < txPerStep; ++i)
    {
        if (mAccountsAvailable.empty() &&
            cfg.mode != LoadGenMode::PAY_PREGENERATED)
        {
            CLOG_WARNING(LoadGen,
                         "Load generation failed: no more accounts available");
            mLoadgenFail.Mark();
            reset();
            return;
        }

        uint64_t sourceAccountId = 0;
        if (cfg.mode != LoadGenMode::PAY_PREGENERATED)
        {
            sourceAccountId = getNextAvailableAccount(ledgerNum);
        }

        std::function<std::pair<TxGenerator::TestAccountPtr,
                                TransactionFrameBaseConstPtr>()>
            generateTx;

        switch (cfg.mode)
        {
        case LoadGenMode::PAY:
        {
            auto byteCount = chooseByteCount(mApp.getConfig());
            generateTx = [&, byteCount]() {
                return mTxGenerator.paymentTransaction(
                    cfg.nAccounts, cfg.offset, ledgerNum, sourceAccountId,
                    byteCount, cfg.maxGeneratedFeeRate);
            };
        }
        break;
        case LoadGenMode::MIXED_CLASSIC:
        {
            auto byteCount = chooseByteCount(mApp.getConfig());
            bool isDex =
                rand_uniform<uint32_t>(1, 100) <= cfg.getDexTxPercent();
            generateTx = [&, byteCount, isDex]() {
                if (isDex)
                {
                    return mTxGenerator.manageOfferTransaction(
                        ledgerNum, sourceAccountId, byteCount,
                        cfg.maxGeneratedFeeRate);
                }
                else
                {
                    return mTxGenerator.paymentTransaction(
                        cfg.nAccounts, cfg.offset, ledgerNum, sourceAccountId,
                        byteCount, cfg.maxGeneratedFeeRate);
                }
            };
        }
        break;
        case LoadGenMode::SOROBAN_UPLOAD:
        {
            generateTx = [&]() {
                return mTxGenerator.sorobanRandomWasmTransaction(
                    ledgerNum, sourceAccountId,
                    mTxGenerator.generateFee(cfg.maxGeneratedFeeRate,
                                             /* opsCnt */ 1));
            };
        }
        break;
        case LoadGenMode::SOROBAN_INVOKE_SETUP:
        case LoadGenMode::SOROBAN_UPGRADE_SETUP:
            generateTx = [&] {
                auto& sorobanCfg = cfg.getMutSorobanConfig();
                if (sorobanCfg.nWasms != 0)
                {
                    --sorobanCfg.nWasms;
                    return createUploadWasmTransaction(cfg, ledgerNum,
                                                       sourceAccountId);
                }
                else
                {
                    --sorobanCfg.nInstances;
                    return createInstanceTransaction(cfg, ledgerNum,
                                                     sourceAccountId);
                }
            };
            break;
        case LoadGenMode::SOROBAN_INVOKE:
            generateTx = [&]() {
                auto instanceIter = mContractInstances.find(sourceAccountId);
                releaseAssert(instanceIter != mContractInstances.end());
                auto const& instance = instanceIter->second;
                return mTxGenerator.invokeSorobanLoadTransaction(
                    ledgerNum, sourceAccountId, instance, mContactOverheadBytes,
                    cfg.maxGeneratedFeeRate);
            };
            break;
        case LoadGenMode::SOROBAN_CREATE_UPGRADE:
            generateTx = [&]() {
                releaseAssert(mCodeKey);
                releaseAssert(mContractInstanceKeys.size() == 1);

                auto upgradeBytes =
                    mTxGenerator.getConfigUpgradeSetFromLoadConfig(
                        cfg.getSorobanUpgradeConfig());
                return mTxGenerator.invokeSorobanCreateUpgradeTransaction(
                    ledgerNum, sourceAccountId, upgradeBytes, *mCodeKey,
                    *mContractInstanceKeys.begin(), cfg.maxGeneratedFeeRate);
            };
            break;
        case LoadGenMode::MIXED_CLASSIC_SOROBAN:
        {
            auto byteCount = chooseByteCount(mApp.getConfig());
            generateTx = [&, byteCount]() {
                return createMixedClassicSorobanTransaction(
                    ledgerNum, sourceAccountId, byteCount, cfg);
            };
        }
        break;
        case LoadGenMode::PAY_PREGENERATED:
            generateTx = [&]() { return readTransactionFromFile(cfg); };
            break;
        case LoadGenMode::SOROBAN_INVOKE_APPLY_LOAD:
            generateTx = [&]() {
                auto instanceIter = mContractInstances.find(sourceAccountId);
                releaseAssert(instanceIter != mContractInstances.end());
                auto const& instance = instanceIter->second;
                auto const& appCfg = mApp.getConfig();
                uint64_t dataEntryCount =
                    appCfg.APPLY_LOAD_BL_BATCH_SIZE *
                    appCfg.APPLY_LOAD_BL_SIMULATED_LEDGERS;
                size_t dataEntrySize =
                    appCfg.APPLY_LOAD_DATA_ENTRY_SIZE_FOR_TESTING;

                return mTxGenerator.invokeSorobanLoadTransactionV2(
                    ledgerNum, sourceAccountId, instance, dataEntryCount,
                    dataEntrySize, cfg.maxGeneratedFeeRate);
            };
            break;
        }

        try
        {
            if (submitTx(cfg, generateTx))
            {
                --cfg.nTxs;
            }
        }
        catch (std::runtime_error const& e)
        {
            CLOG_ERROR(LoadGen, "Exception while submitting tx: {}", e.what());
            mFailed = true;
            break;
        }

        if (mFailed)
        {
            break;
        }
        ++count;
        if (cfg.nAccounts == 0 || !cfg.areTxsRemaining())
        {
            // Nothing to do for the rest of the step
            break;
        }
    }

    auto submit = submitScope.Stop();

    now = mApp.timeNow();

    // Emit a log message once per second.
    if (now != mLastSecond)
    {
        logProgress(submit, cfg);
    }

    mLastSecond = now;
    mTotalSubmitted += count;
    scheduleLoadGeneration(cfg);
}

bool
LoadGenerator::submitTx(GeneratedLoadConfig const& cfg,
                        std::function<std::pair<TxGenerator::TestAccountPtr,
                                                TransactionFrameBaseConstPtr>()>
                            generateTx)
{
    auto [from, tx] = generateTx();

    TransactionResultCode code;
    TransactionQueue::AddResultCode status;
    uint32_t numTries = 0;

    while ((status = execute(tx, cfg.mode, code)) !=
           TransactionQueue::AddResultCode::ADD_STATUS_PENDING)
    {

        if (cfg.mode != LoadGenMode::PAY_PREGENERATED && cfg.skipLowFeeTxs &&
            (status ==
                 TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER ||
             (status == TransactionQueue::AddResultCode::ADD_STATUS_ERROR &&
              code == txINSUFFICIENT_FEE)))
        {
            // Rollback the seq num of the test account as we regenerate the
            // transaction.
            from->setSequenceNumber(from->getLastSequenceNumber() - 1);
            CLOG_INFO(LoadGen, "skipped low fee tx with fee {}",
                      tx->getInclusionFee());
            return false;
        }

        // No re-submission in PAY_PREGENERATED mode.
        // Each transaction is for a unique source account, so we
        // should not see BAD_SEQ error codes unless core is actually dropping
        // txs due to overload (in which case we should just fail loadgen,
        // instead of re-submitting)
        if (++numTries >= TX_SUBMIT_MAX_TRIES ||
            status != TransactionQueue::AddResultCode::ADD_STATUS_ERROR ||
            cfg.mode == LoadGenMode::PAY_PREGENERATED)
        {
            mFailed = true;
            return false;
        }

        // In case of bad seqnum, attempt refreshing it from the DB
        maybeHandleFailedTx(tx, from, status, code); // Update seq num

        // Regenerate a new payment tx
        std::tie(from, tx) = generateTx();
    }

    return true;
}

uint64_t
LoadGenerator::getNextAvailableAccount(uint32_t ledgerNum)
{
    uint64_t sourceAccountId;
    do
    {
        releaseAssert(!mAccountsAvailable.empty());

        auto sourceAccountIdx =
            rand_uniform<uint64_t>(0, mAccountsAvailable.size() - 1);
        auto it = mAccountsAvailable.begin();
        std::advance(it, sourceAccountIdx);
        sourceAccountId = *it;
        mAccountsAvailable.erase(it);
        releaseAssert(mAccountsInUse.insert(sourceAccountId).second);

        // Although mAccountsAvailable shouldn't contain pending accounts, it is
        // possible when the network is overloaded. Consider the following
        // scenario:
        // 1. This node generates a transaction `t` using account `a` and
        //    broadcasts it on. In doing so, loadgen marks `a` as in use,
        //    removing it from `mAccountsAvailable.
        // 2. For whatever reason, `t` never makes it out of the queue and this
        //    node bans it.
        // 3. After some period of time, this node unbans `t` because bans only
        //    last for so many ledgers.
        // 4. Loadgen marks `a` available, moving it back into
        //    `mAccountsAvailable`.
        // 5. This node hears about `t` again on the network and (as it is no
        //    longer banned) adds it back to the queue
        // 6. getNextAvailableAccount draws `a` from `mAccountsAvailable`.
        //    However, `a` is no longer available as `t` is in the transaction
        //    queue!
        //
        // In this scenario, returning `a` results in an assertion failure
        // later. To resolve this, we resample a new account by simply looping
        // here.
    } while (mApp.getHerder().sourceAccountPending(
        mTxGenerator.findAccount(sourceAccountId, ledgerNum)->getPublicKey()));

    return sourceAccountId;
}

void
LoadGenerator::logProgress(std::chrono::nanoseconds submitTimer,
                           GeneratedLoadConfig const& cfg) const
{
    using namespace std::chrono;

    auto& applyTx = mApplyTxTimer;
    auto& applyOp = mApplyOpTimer;

    auto submitSteps = duration_cast<milliseconds>(submitTimer).count();

    auto remainingTxCount = 0;
    if (cfg.isSorobanSetup())
    {
        remainingTxCount =
            cfg.getSorobanConfig().nWasms + cfg.getSorobanConfig().nInstances;
    }
    else
    {
        remainingTxCount = cfg.nTxs;
    }

    auto etaSecs = (uint32_t)(((double)remainingTxCount) /
                              max<double>(1, applyTx.one_minute_rate()));

    auto etaHours = etaSecs / 3600;
    auto etaMins = etaSecs % 3600 / 60;

    if (cfg.isSoroban())
    {
        CLOG_INFO(LoadGen,
                  "Tx/s: {} target, {} tx actual (1m EWMA). Pending: {} txs. "
                  "ETA: {}h{}m",
                  cfg.txRate, applyTx.one_minute_rate(), remainingTxCount,
                  etaHours, etaMins);
    }
    else
    {
        CLOG_INFO(LoadGen,
                  "Tx/s: {} target, {}tx/{}op actual (1m EWMA). Pending: {} "
                  "txs. ETA: {}h{}m",
                  cfg.txRate, applyTx.one_minute_rate(),
                  applyOp.one_minute_rate(), remainingTxCount, etaHours,
                  etaMins);
    }

    CLOG_DEBUG(LoadGen, "Step timing: {}ms submit.", submitSteps);

    mTxMetrics.report();
}

std::pair<TxGenerator::TestAccountPtr, TransactionFrameBaseConstPtr>
LoadGenerator::createMixedClassicSorobanTransaction(
    uint32_t ledgerNum, uint64_t sourceAccountId, uint32_t classicByteCount,
    GeneratedLoadConfig const& cfg)
{
    auto const& mixCfg = cfg.getMixClassicSorobanConfig();
    std::discrete_distribution<uint32_t> dist({mixCfg.payWeight,
                                               mixCfg.sorobanUploadWeight,
                                               mixCfg.sorobanInvokeWeight});
    switch (dist(getGlobalRandomEngine()))
    {
    case 0:
    {
        // Create a payment transaction
        mLastMixedMode = LoadGenMode::PAY;
        return mTxGenerator.paymentTransaction(
            cfg.nAccounts, cfg.offset, ledgerNum, sourceAccountId,
            classicByteCount, cfg.maxGeneratedFeeRate);
    }
    case 1:
    {
        // Create a soroban upload transaction
        mLastMixedMode = LoadGenMode::SOROBAN_UPLOAD;
        return mTxGenerator.sorobanRandomWasmTransaction(
            ledgerNum, sourceAccountId,
            mTxGenerator.generateFee(cfg.maxGeneratedFeeRate, /* opsCnt */ 1));
    }
    case 2:
    {
        // Create a soroban invoke transaction
        mLastMixedMode = LoadGenMode::SOROBAN_INVOKE;

        auto instanceIter = mContractInstances.find(sourceAccountId);
        releaseAssert(instanceIter != mContractInstances.end());
        return mTxGenerator.invokeSorobanLoadTransaction(
            ledgerNum, sourceAccountId, instanceIter->second,
            mContactOverheadBytes, cfg.maxGeneratedFeeRate);
    }
    default:
        releaseAssert(false);
    }
}

std::pair<TxGenerator::TestAccountPtr, TransactionFrameBaseConstPtr>
LoadGenerator::createUploadWasmTransaction(GeneratedLoadConfig const& cfg,
                                           uint32_t ledgerNum,
                                           uint64_t sourceAccountId)
{
    auto wasm = cfg.modeSetsUpInvoke() ? rust_bridge::get_test_wasm_loadgen()
                                       : rust_bridge::get_write_bytes();
    xdr::opaque_vec<> wasmBytes;
    wasmBytes.assign(wasm.data.begin(), wasm.data.end());

    LedgerKey contractCodeLedgerKey;
    contractCodeLedgerKey.type(CONTRACT_CODE);
    contractCodeLedgerKey.contractCode().hash = sha256(wasmBytes);

    releaseAssert(!mCodeKey);
    mCodeKey = contractCodeLedgerKey;

    // Wasm blob + approximate overhead for contract
    // instance and ContractCode LE overhead
    mContactOverheadBytes = wasmBytes.size() + 160;

    return mTxGenerator.createUploadWasmTransaction(ledgerNum, sourceAccountId,
                                                    wasmBytes, *mCodeKey,
                                                    cfg.maxGeneratedFeeRate);
}

std::pair<TxGenerator::TestAccountPtr, TransactionFrameBaseConstPtr>
LoadGenerator::createInstanceTransaction(GeneratedLoadConfig const& cfg,
                                         uint32_t ledgerNum,
                                         uint64_t sourceAccountId)
{
    auto salt = sha256("upgrade" +
                       std::to_string(++mNumCreateContractTransactionCalls));

    auto txPair = mTxGenerator.createContractTransaction(
        ledgerNum, sourceAccountId, *mCodeKey, mContactOverheadBytes, salt,
        cfg.maxGeneratedFeeRate);

    auto const& instanceLk =
        txPair.second->sorobanResources().footprint.readWrite.back();

    mContractInstanceKeys.emplace(instanceLk);

    return txPair;
}

void
LoadGenerator::maybeHandleFailedTx(TransactionFrameBaseConstPtr tx,
                                   TxGenerator::TestAccountPtr sourceAccount,
                                   TransactionQueue::AddResultCode status,
                                   TransactionResultCode code)
{
    // Note that if transaction is a DUPLICATE, its sequence number is
    // incremented on the next call to execute.
    if (status == TransactionQueue::AddResultCode::ADD_STATUS_ERROR &&
        code == txBAD_SEQ)
    {
        auto txQueueSeqNum =
            tx->isSoroban()
                ? mApp.getHerder()
                      .getSorobanTransactionQueue()
                      .getInQueueSeqNum(sourceAccount->getPublicKey())
                : mApp.getHerder().getTransactionQueue().getInQueueSeqNum(
                      sourceAccount->getPublicKey());
        if (txQueueSeqNum)
        {
            sourceAccount->setSequenceNumber(*txQueueSeqNum);
            return;
        }
        if (!mTxGenerator.loadAccount(sourceAccount))
        {
            CLOG_ERROR(LoadGen, "Unable to reload account {}",
                       sourceAccount->getAccountId());
        }
    }
}

std::vector<LedgerKey>
LoadGenerator::checkSorobanStateSynced(Application& app,
                                       GeneratedLoadConfig const& cfg)
{
    // We don't care if TXs succeed for SOROBAN_UPLOAD
    if (!cfg.isSoroban() || cfg.mode == LoadGenMode::SOROBAN_UPLOAD)
    {
        return {};
    }

    std::vector<LedgerKey> result;
    LedgerSnapshot lsg(mApp);
    for (auto const& lk : mContractInstanceKeys)
    {
        if (!lsg.load(lk))
        {
            result.emplace_back(lk);
        }
    }

    if (mCodeKey && !lsg.load(*mCodeKey))
    {
        result.emplace_back(*mCodeKey);
    }

    return result;
}

std::vector<TxGenerator::TestAccountPtr>
LoadGenerator::checkAccountSynced(Application& app)
{
    std::vector<TxGenerator::TestAccountPtr> result;
    for (auto const& acc : mTxGenerator.getAccounts())
    {
        TxGenerator::TestAccountPtr account = acc.second;
        auto accountFromDB = *account;

        auto reloadRes = mTxGenerator.loadAccount(accountFromDB);
        // Ensure that the sequence number matches expected
        // seqnum. Timeout after 20 ledgers.
        if (!reloadRes)
        {
            auto msg =
                fmt::format("Account {} used to submit payment tx could not "
                            "load, DB might be in a corrupted state",
                            account->getAccountId());
            throw std::runtime_error(msg);
        }
        else if (account->getLastSequenceNumber() !=
                 accountFromDB.getLastSequenceNumber())
        {
            CLOG_TRACE(LoadGen,
                       "Account {} is at sequence num {}, but the DB is at  {}",
                       account->getAccountId(),
                       account->getLastSequenceNumber(),
                       accountFromDB.getLastSequenceNumber());
            result.push_back(account);
        }
    }
    return result;
}

bool
LoadGenerator::checkMinimumSorobanSuccess(GeneratedLoadConfig const& cfg)
{
    if (!cfg.isSoroban())
    {
        // Only applies to soroban modes
        return true;
    }

    int64_t nTxns = mTxGenerator.getApplySorobanSuccess().count() +
                    mTxGenerator.getApplySorobanFailure().count() -
                    mPreLoadgenApplySorobanSuccess -
                    mPreLoadgenApplySorobanFailure;

    if (nTxns == 0)
    {
        // Special case to avoid division by zero
        return true;
    }

    int64_t nSuccessful = mTxGenerator.getApplySorobanSuccess().count() -
                          mPreLoadgenApplySorobanSuccess;
    return (nSuccessful * 100) / nTxns >= cfg.getMinSorobanPercentSuccess();
}

void
LoadGenerator::waitTillComplete(GeneratedLoadConfig cfg)
{
    if (!mLoadTimer)
    {
        mLoadTimer = std::make_unique<VirtualTimer>(mApp.getClock());
    }

    bool classicIsDone = false;
    bool sorobanIsDone = false;
    if (mApp.getRunInOverlayOnlyMode())
    {
        auto count = getTxCount(mApp, cfg.isSoroban());
        CLOG_INFO(LoadGen, "Transaction count: {}", count);
        CLOG_INFO(LoadGen, "Transactions applied at the start: {}",
                  mTransactionsAppliedAtTheStart);
        CLOG_INFO(LoadGen, "Transactions applied: {}", mTotalSubmitted);
        classicIsDone =
            (count - mTransactionsAppliedAtTheStart) == mTotalSubmitted;
        sorobanIsDone = classicIsDone;
    }
    else
    {
        classicIsDone = checkAccountSynced(mApp).empty();
        sorobanIsDone = checkSorobanStateSynced(mApp, cfg).empty();
    }

    // If there are no inconsistencies and we have generated all load, finish
    if (classicIsDone && sorobanIsDone && cfg.isDone())
    {
        // Check whether run met the minimum success rate for soroban invoke
        if (checkMinimumSorobanSuccess(cfg))
        {
            CLOG_INFO(LoadGen, "Load generation complete.");
            mLoadgenComplete.Mark();
            reset();
        }
        else
        {
            CLOG_INFO(LoadGen, "Load generation failed to meet minimum success "
                               "rate for soroban transactions.");
            // In this case the soroban setup phase executed successfully (as
            // indicated by the lack of entries in `sorobanInconsistencies`), so
            // the soroban persistent state does not need to be reset.
            emitFailure(false);
        }
        return;
    }
    // If we have an inconsistency, reset the timer and wait for another ledger
    else if (!classicIsDone || !sorobanIsDone)
    {
        if (++mWaitTillCompleteForLedgers >= TIMEOUT_NUM_LEDGERS)
        {
            emitFailure(!sorobanIsDone);
            return;
        }

        mLoadTimer->expires_from_now(
            mApp.getLedgerManager().getExpectedLedgerCloseTime());
        mLoadTimer->async_wait([this, cfg]() { this->waitTillComplete(cfg); },
                               &VirtualTimer::onFailureNoop);
    }
    // If there are no inconsistencies but we aren't done yet, we have more load
    // to generate, so schedule loadgen
    else
    {
        // If there are no inconsistencies but we aren't done, we must be in a
        // two phase mode (soroban setup).
        releaseAssert(cfg.isSorobanSetup());

        // All Wasms should be deployed
        releaseAssert(cfg.getSorobanConfig().nWasms == 0);

        // 1 deploy TX per instance
        cfg.nTxs = cfg.getSorobanConfig().nInstances;
        scheduleLoadGeneration(cfg);
    }
}

void
LoadGenerator::emitFailure(bool resetSoroban)
{
    CLOG_INFO(LoadGen, "Load generation failed.");
    mLoadgenFail.Mark();
    reset();
    if (resetSoroban)
    {
        resetSorobanState();
    }
}

void
LoadGenerator::waitTillCompleteWithoutChecks()
{
    if (!mLoadTimer)
    {
        mLoadTimer = std::make_unique<VirtualTimer>(mApp.getClock());
    }
    if (++mWaitTillCompleteForLedgers == COMPLETION_TIMEOUT_WITHOUT_CHECKS)
    {
        auto inconsistencies = checkAccountSynced(mApp);
        CLOG_INFO(LoadGen, "Load generation complete.");
        if (!inconsistencies.empty())
        {
            CLOG_INFO(
                LoadGen,
                "{} account seq nums are not in sync with db; this is expected "
                "for high traffic due to tx queue limiter evictions.",
                inconsistencies.size());
        }
        mLoadgenComplete.Mark();
        reset();
        return;
    }
    mLoadTimer->expires_from_now(
        mApp.getLedgerManager().getExpectedLedgerCloseTime());
    mLoadTimer->async_wait([this]() { this->waitTillCompleteWithoutChecks(); },
                           &VirtualTimer::onFailureNoop);
}

LoadGenerator::TxMetrics::TxMetrics(medida::MetricsRegistry& m)
    : mNativePayment(m.NewMeter({"loadgen", "payment", "submitted"}, "op"))
    , mManageOfferOps(m.NewMeter({"loadgen", "manageoffer", "submitted"}, "op"))
    , mSorobanUploadTxs(m.NewMeter({"loadgen", "soroban", "upload"}, "txn"))
    , mSorobanSetupInvokeTxs(
          m.NewMeter({"loadgen", "soroban", "setup_invoke"}, "txn"))
    , mSorobanSetupUpgradeTxs(
          m.NewMeter({"loadgen", "soroban", "setup_upgrade"}, "txn"))
    , mSorobanInvokeTxs(m.NewMeter({"loadgen", "soroban", "invoke"}, "txn"))
    , mSorobanCreateUpgradeTxs(
          m.NewMeter({"loadgen", "soroban", "create_upgrade"}, "txn"))
    , mTxnAttempted(m.NewMeter({"loadgen", "txn", "attempted"}, "txn"))
    , mTxnRejected(m.NewMeter({"loadgen", "txn", "rejected"}, "txn"))
    , mTxnBytes(m.NewMeter({"loadgen", "txn", "bytes"}, "txn"))
{
}

void
LoadGenerator::TxMetrics::report()
{
    CLOG_DEBUG(LoadGen,
               "Counts: {} tx, {} rj, {} by, {} na, {} dex, {} "
               "su, {} ssi, {} ssu, {} si, {} scu",
               mTxnAttempted.count(), mTxnRejected.count(), mTxnBytes.count(),
               mNativePayment.count(), mManageOfferOps.count(),
               mSorobanUploadTxs.count(), mSorobanSetupInvokeTxs.count(),
               mSorobanSetupUpgradeTxs.count(), mSorobanInvokeTxs.count(),
               mSorobanCreateUpgradeTxs.count());

    CLOG_DEBUG(LoadGen,
               "Rates/sec (1m EWMA): {} tx, {} rj, {} by, {} na, "
               "{} dex, {} su, {} ssi, {} ssu, {} si, {} scu",
               mTxnAttempted.one_minute_rate(), mTxnRejected.one_minute_rate(),
               mTxnBytes.one_minute_rate(), mNativePayment.one_minute_rate(),
               mManageOfferOps.one_minute_rate(),
               mSorobanUploadTxs.one_minute_rate(),
               mSorobanSetupInvokeTxs.one_minute_rate(),
               mSorobanSetupUpgradeTxs.one_minute_rate(),
               mSorobanInvokeTxs.one_minute_rate(),
               mSorobanCreateUpgradeTxs.one_minute_rate());
}

TransactionQueue::AddResultCode
LoadGenerator::execute(TransactionFrameBasePtr txf, LoadGenMode mode,
                       TransactionResultCode& code)
{
    TxMetrics txm(mApp.getMetrics());

    // Record tx metrics.
    switch (mode)
    {
    case LoadGenMode::PAY:
    case LoadGenMode::PAY_PREGENERATED:
        txm.mNativePayment.Mark(txf->getNumOperations());
        break;
    case LoadGenMode::MIXED_CLASSIC:
        if (txf->hasDexOperations())
        {
            txm.mManageOfferOps.Mark(txf->getNumOperations());
        }
        else
        {
            txm.mNativePayment.Mark(txf->getNumOperations());
        }
        break;
    case LoadGenMode::SOROBAN_UPLOAD:
        txm.mSorobanUploadTxs.Mark();
        break;
    case LoadGenMode::SOROBAN_INVOKE_SETUP:
        txm.mSorobanSetupInvokeTxs.Mark();
        break;
    case LoadGenMode::SOROBAN_UPGRADE_SETUP:
        txm.mSorobanSetupUpgradeTxs.Mark();
        break;
    case LoadGenMode::SOROBAN_INVOKE:
        txm.mSorobanInvokeTxs.Mark();
        break;
    case LoadGenMode::SOROBAN_CREATE_UPGRADE:
        txm.mSorobanCreateUpgradeTxs.Mark();
        break;
    case LoadGenMode::MIXED_CLASSIC_SOROBAN:
        switch (mLastMixedMode)
        {
        case LoadGenMode::PAY:
            txm.mNativePayment.Mark(txf->getNumOperations());
            break;
        case LoadGenMode::SOROBAN_UPLOAD:
            txm.mSorobanUploadTxs.Mark();
            break;
        case LoadGenMode::SOROBAN_INVOKE:
            txm.mSorobanInvokeTxs.Mark();
            break;
        default:
            releaseAssert(false);
        }
        break;
    case LoadGenMode::SOROBAN_INVOKE_APPLY_LOAD:
        txm.mSorobanInvokeTxs.Mark();
        break;
    }

    txm.mTxnAttempted.Mark();

    auto msg = txf->toStellarMessage();
    txm.mTxnBytes.Mark(xdr::xdr_argpack_size(*msg));

    // Skip certain checks for pregenerated transactions
    bool isPregeneratedTx = (mode == LoadGenMode::PAY_PREGENERATED);
    auto addResult =
        mApp.getHerder().recvTransaction(txf, true, isPregeneratedTx);
    if (addResult.code != TransactionQueue::AddResultCode::ADD_STATUS_PENDING)
    {

        auto resultStr = addResult.txResult
                             ? xdrToCerealString(addResult.txResult->getXDR(),
                                                 "TransactionResult")
                             : "";
        CLOG_INFO(LoadGen, "tx rejected '{}': ===> {}, {}",
                  TX_STATUS_STRING[static_cast<int>(addResult.code)],
                  txf->isSoroban() ? "soroban"
                                   : xdrToCerealString(txf->getEnvelope(),
                                                       "TransactionEnvelope"),
                  resultStr);
        if (addResult.code == TransactionQueue::AddResultCode::ADD_STATUS_ERROR)
        {
            releaseAssert(addResult.txResult);
            code = addResult.txResult->getResultCode();
        }
        txm.mTxnRejected.Mark();
    }
    else
    {
        mApp.getOverlayManager().broadcastMessage(msg, txf->getFullHash());
    }

    return addResult.code;
}

void
GeneratedLoadConfig::copySorobanNetworkConfigToUpgradeConfig(
    SorobanNetworkConfig const& baseConfig,
    SorobanNetworkConfig const& updatedConfig)
{
    // TODO: this whole function has to be rewritten to only set the upgrades
    // when there is a diff between base and updated configs. Moreover,
    // `upgradeCfg` should store whole XDR structs instead of individual
    // fields, as upgrade is performed on the entire struct.
    releaseAssert(mode == LoadGenMode::SOROBAN_CREATE_UPGRADE);
    auto& upgradeCfg = getMutSorobanUpgradeConfig();

    upgradeCfg.maxContractSizeBytes = updatedConfig.maxContractSizeBytes();
    upgradeCfg.maxContractDataKeySizeBytes =
        updatedConfig.maxContractDataKeySizeBytes();
    upgradeCfg.maxContractDataEntrySizeBytes =
        updatedConfig.maxContractDataEntrySizeBytes();

    upgradeCfg.ledgerMaxInstructions = updatedConfig.ledgerMaxInstructions();
    upgradeCfg.txMaxInstructions = updatedConfig.txMaxInstructions();
    upgradeCfg.feeRatePerInstructionsIncrement =
        updatedConfig.feeRatePerInstructionsIncrement();
    upgradeCfg.txMemoryLimit = updatedConfig.txMemoryLimit();
    if (baseConfig.cpuCostParams() != updatedConfig.cpuCostParams())
    {
        upgradeCfg.cpuCostParams = updatedConfig.cpuCostParams();
    }
    if (baseConfig.memCostParams() != updatedConfig.memCostParams())
    {
        upgradeCfg.memCostParams = updatedConfig.memCostParams();
    }

    upgradeCfg.ledgerMaxDiskReadEntries =
        updatedConfig.ledgerMaxDiskReadEntries();
    upgradeCfg.ledgerMaxDiskReadBytes = updatedConfig.ledgerMaxDiskReadBytes();
    upgradeCfg.ledgerMaxWriteLedgerEntries =
        updatedConfig.ledgerMaxWriteLedgerEntries();
    upgradeCfg.ledgerMaxWriteBytes = updatedConfig.ledgerMaxWriteBytes();
    upgradeCfg.ledgerMaxTxCount = updatedConfig.ledgerMaxTxCount();
    upgradeCfg.feeDiskReadLedgerEntry = updatedConfig.feeDiskReadLedgerEntry();
    upgradeCfg.feeWriteLedgerEntry = updatedConfig.feeWriteLedgerEntry();
    upgradeCfg.feeDiskRead1KB = updatedConfig.feeDiskRead1KB();
    upgradeCfg.feeFlatRateWrite1KB = updatedConfig.feeFlatRateWrite1KB();
    upgradeCfg.txMaxDiskReadEntries = updatedConfig.txMaxDiskReadEntries();
    upgradeCfg.txMaxFootprintEntries = updatedConfig.txMaxFootprintEntries();
    upgradeCfg.txMaxDiskReadBytes = updatedConfig.txMaxDiskReadBytes();
    upgradeCfg.txMaxWriteLedgerEntries =
        updatedConfig.txMaxWriteLedgerEntries();
    upgradeCfg.txMaxWriteBytes = updatedConfig.txMaxWriteBytes();

    upgradeCfg.feeHistorical1KB = updatedConfig.feeHistorical1KB();

    upgradeCfg.txMaxContractEventsSizeBytes =
        updatedConfig.txMaxContractEventsSizeBytes();

    upgradeCfg.ledgerMaxTransactionsSizeBytes =
        updatedConfig.ledgerMaxTransactionSizesBytes();
    upgradeCfg.txMaxSizeBytes = updatedConfig.txMaxSizeBytes();
    upgradeCfg.feeTransactionSize1KB = updatedConfig.feeTransactionSize1KB();

    upgradeCfg.maxEntryTTL = updatedConfig.stateArchivalSettings().maxEntryTTL;
    upgradeCfg.minTemporaryTTL =
        updatedConfig.stateArchivalSettings().minTemporaryTTL;
    upgradeCfg.minPersistentTTL =
        updatedConfig.stateArchivalSettings().minPersistentTTL;
    upgradeCfg.persistentRentRateDenominator =
        updatedConfig.stateArchivalSettings().persistentRentRateDenominator;
    upgradeCfg.tempRentRateDenominator =
        updatedConfig.stateArchivalSettings().tempRentRateDenominator;
    upgradeCfg.maxEntriesToArchive =
        updatedConfig.stateArchivalSettings().maxEntriesToArchive;
    upgradeCfg.liveSorobanStateSizeWindowSampleSize =
        updatedConfig.stateArchivalSettings()
            .liveSorobanStateSizeWindowSampleSize;
    upgradeCfg.liveSorobanStateSizeWindowSamplePeriod =
        updatedConfig.stateArchivalSettings()
            .liveSorobanStateSizeWindowSamplePeriod;
    upgradeCfg.evictionScanSize =
        updatedConfig.stateArchivalSettings().evictionScanSize;
    upgradeCfg.startingEvictionScanLevel =
        updatedConfig.stateArchivalSettings().startingEvictionScanLevel;

    upgradeCfg.rentFee1KBSorobanStateSizeLow =
        updatedConfig.rentFee1KBSorobanStateSizeLow();
    upgradeCfg.rentFee1KBSorobanStateSizeHigh =
        updatedConfig.rentFee1KBSorobanStateSizeHigh();

    upgradeCfg.ledgerMaxDependentTxClusters =
        updatedConfig.ledgerMaxDependentTxClusters();

    upgradeCfg.ledgerTargetCloseTimeMilliseconds =
        updatedConfig.ledgerTargetCloseTimeMilliseconds();
    upgradeCfg.nominationTimeoutInitialMilliseconds =
        updatedConfig.nominationTimeoutInitialMilliseconds();
    upgradeCfg.nominationTimeoutIncrementMilliseconds =
        updatedConfig.nominationTimeoutIncrementMilliseconds();
    upgradeCfg.ballotTimeoutInitialMilliseconds =
        updatedConfig.ballotTimeoutInitialMilliseconds();
    upgradeCfg.ballotTimeoutIncrementMilliseconds =
        updatedConfig.ballotTimeoutIncrementMilliseconds();
    upgradeCfg.nominationTimeoutInitialMilliseconds =
        updatedConfig.nominationTimeoutInitialMilliseconds();
    upgradeCfg.nominationTimeoutIncrementMilliseconds =
        updatedConfig.nominationTimeoutIncrementMilliseconds();
}

GeneratedLoadConfig
GeneratedLoadConfig::createSorobanInvokeSetupLoad(uint32_t nAccounts,
                                                  uint32_t nInstances,
                                                  uint32_t txRate)
{
    GeneratedLoadConfig cfg;
    cfg.mode = LoadGenMode::SOROBAN_INVOKE_SETUP;
    cfg.nAccounts = nAccounts;
    cfg.getMutSorobanConfig().nInstances = nInstances;
    cfg.txRate = txRate;
    return cfg;
}

GeneratedLoadConfig
GeneratedLoadConfig::createSorobanUpgradeSetupLoad()
{
    GeneratedLoadConfig cfg;
    cfg.mode = LoadGenMode::SOROBAN_UPGRADE_SETUP;
    cfg.nAccounts = 1;
    cfg.getMutSorobanConfig().nInstances = 1;
    cfg.txRate = 1;
    return cfg;
}

GeneratedLoadConfig
GeneratedLoadConfig::txLoad(LoadGenMode mode, uint32_t nAccounts, uint32_t nTxs,
                            uint32_t txRate, uint32_t offset,
                            std::optional<uint32_t> maxFee)
{
    GeneratedLoadConfig cfg;
    cfg.mode = mode;
    cfg.nAccounts = nAccounts;
    cfg.nTxs = nTxs;
    cfg.txRate = txRate;
    cfg.offset = offset;
    cfg.maxGeneratedFeeRate = maxFee;
    return cfg;
}

GeneratedLoadConfig
GeneratedLoadConfig::pregeneratedTxLoad(uint32_t nAccounts, uint32_t nTxs,
                                        uint32_t txRate, uint32_t offset,
                                        std::filesystem::path const& file)
{
    GeneratedLoadConfig cfg;
    cfg.mode = LoadGenMode::PAY_PREGENERATED;
    cfg.nAccounts = nAccounts;
    cfg.nTxs = nTxs;
    cfg.txRate = txRate;
    cfg.offset = offset;
    cfg.preloadedTransactionsFile = file;
    cfg.skipLowFeeTxs = false;
    return cfg;
}

GeneratedLoadConfig::SorobanConfig&
GeneratedLoadConfig::getMutSorobanConfig()
{
    releaseAssert(isSoroban() && mode != LoadGenMode::SOROBAN_UPLOAD);
    return sorobanConfig;
}

GeneratedLoadConfig::SorobanConfig const&
GeneratedLoadConfig::getSorobanConfig() const
{
    releaseAssert(isSoroban() && mode != LoadGenMode::SOROBAN_UPLOAD);
    return sorobanConfig;
}

SorobanUpgradeConfig&
GeneratedLoadConfig::getMutSorobanUpgradeConfig()
{
    releaseAssert(mode == LoadGenMode::SOROBAN_CREATE_UPGRADE);
    return sorobanUpgradeConfig;
}

SorobanUpgradeConfig const&
GeneratedLoadConfig::getSorobanUpgradeConfig() const
{
    releaseAssert(mode == LoadGenMode::SOROBAN_CREATE_UPGRADE);
    return sorobanUpgradeConfig;
}

GeneratedLoadConfig::MixClassicSorobanConfig&
GeneratedLoadConfig::getMutMixClassicSorobanConfig()
{
    releaseAssert(mode == LoadGenMode::MIXED_CLASSIC_SOROBAN);
    return mixClassicSorobanConfig;
}

GeneratedLoadConfig::MixClassicSorobanConfig const&
GeneratedLoadConfig::getMixClassicSorobanConfig() const
{
    releaseAssert(mode == LoadGenMode::MIXED_CLASSIC_SOROBAN);
    return mixClassicSorobanConfig;
}

uint32_t&
GeneratedLoadConfig::getMutDexTxPercent()
{
    releaseAssert(mode == LoadGenMode::MIXED_CLASSIC);
    return dexTxPercent;
}

uint32_t const&
GeneratedLoadConfig::getDexTxPercent() const
{
    releaseAssert(mode == LoadGenMode::MIXED_CLASSIC);
    return dexTxPercent;
}

uint32_t
GeneratedLoadConfig::getMinSorobanPercentSuccess() const
{
    releaseAssert(isSoroban());
    return mMinSorobanPercentSuccess;
}

void
GeneratedLoadConfig::setMinSorobanPercentSuccess(uint32_t percent)
{
    releaseAssert(isSoroban());
    if (percent > 100)
    {
        throw std::invalid_argument("percent must be <= 100");
    }
    mMinSorobanPercentSuccess = percent;
}

bool
GeneratedLoadConfig::isSoroban() const
{
    return mode == LoadGenMode::SOROBAN_INVOKE ||
           mode == LoadGenMode::SOROBAN_INVOKE_SETUP ||
           mode == LoadGenMode::SOROBAN_UPLOAD ||
           mode == LoadGenMode::SOROBAN_UPGRADE_SETUP ||
           mode == LoadGenMode::SOROBAN_CREATE_UPGRADE ||
           mode == LoadGenMode::MIXED_CLASSIC_SOROBAN ||
           mode == LoadGenMode::SOROBAN_INVOKE_APPLY_LOAD;
}

bool
GeneratedLoadConfig::isSorobanSetup() const
{
    return mode == LoadGenMode::SOROBAN_INVOKE_SETUP ||
           mode == LoadGenMode::SOROBAN_UPGRADE_SETUP;
}

bool
GeneratedLoadConfig::isLoad() const
{
    return mode == LoadGenMode::PAY || mode == LoadGenMode::MIXED_CLASSIC ||
           mode == LoadGenMode::SOROBAN_UPLOAD ||
           mode == LoadGenMode::SOROBAN_INVOKE ||
           mode == LoadGenMode::SOROBAN_CREATE_UPGRADE ||
           mode == LoadGenMode::MIXED_CLASSIC_SOROBAN ||
           mode == LoadGenMode::PAY_PREGENERATED ||
           mode == LoadGenMode::SOROBAN_INVOKE_APPLY_LOAD;
}

bool
GeneratedLoadConfig::modeInvokes() const
{
    return mode == LoadGenMode::SOROBAN_INVOKE ||
           mode == LoadGenMode::MIXED_CLASSIC_SOROBAN ||
           mode == LoadGenMode::SOROBAN_INVOKE_APPLY_LOAD;
}

bool
GeneratedLoadConfig::modeSetsUpInvoke() const
{
    return mode == LoadGenMode::SOROBAN_INVOKE_SETUP;
}

bool
GeneratedLoadConfig::modeUploads() const
{
    return mode == LoadGenMode::SOROBAN_UPLOAD ||
           mode == LoadGenMode::MIXED_CLASSIC_SOROBAN;
}

std::pair<TxGenerator::TestAccountPtr, TransactionFrameBaseConstPtr>
LoadGenerator::readTransactionFromFile(GeneratedLoadConfig const& cfg)
{
    ZoneScoped;

    // Read the next transaction from the file
    TransactionEnvelope txEnv;
    releaseAssert(mPreloadedTransactionsFile);
    if (!mPreloadedTransactionsFile->readOne(txEnv))
    {
        throw std::runtime_error("LoadGenerator: End of file reached, more "
                                 "transactions are needed");
    }

    // Create a TransactionFrame from the envelope
    auto txFrame = TransactionFrameBase::makeTransactionFromWire(
        mApp.getNetworkID(), txEnv);

    // Increment the sequence number of the source account
    auto idx = mCurrPreloadedTransaction % cfg.nAccounts;
    auto acc = mTxGenerator.getAccount(idx + cfg.offset);
    releaseAssert(acc);
    acc->setSequenceNumber(txFrame->getSeqNum());
    ++mCurrPreloadedTransaction;

    // Do not provide an account
    return std::make_pair(nullptr, txFrame);
}
}
