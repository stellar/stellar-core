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
constexpr unsigned short DEFAULT_OP_COUNT = 1;
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
    return values.at(distribution(gRandomEngine));
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
    , mLoadgenComplete(
          mApp.getMetrics().NewMeter({"loadgen", "run", "complete"}, "run"))
    , mLoadgenFail(
          mApp.getMetrics().NewMeter({"loadgen", "run", "failed"}, "run"))
{
}

LoadGenMode
LoadGenerator::getMode(std::string const& mode)
{
    if (mode == "create")
    {
        return LoadGenMode::CREATE;
    }
    else if (mode == "pay")
    {
        return LoadGenMode::PAY;
    }
    else if (mode == "pretend")
    {
        return LoadGenMode::PRETEND;
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
    else
    {
        throw std::runtime_error(
            fmt::format(FMT_STRING("Unknown loadgen mode: {}"), mode));
    }
}

void
LoadGenerator::createRootAccount()
{
    releaseAssert(!mRoot);
    auto rootTestAccount = TestAccount::createRoot(mApp);
    mRoot = make_shared<TestAccount>(rootTestAccount);
    if (!mTxGenerator.loadAccount(mRoot))
    {
        CLOG_ERROR(LoadGen, "Could not retrieve root account!");
    }
}

unsigned short
LoadGenerator::chooseOpCount(Config const& cfg) const
{
    return sampleDiscrete(cfg.LOADGEN_OP_COUNT_FOR_TESTING,
                          cfg.LOADGEN_OP_COUNT_DISTRIBUTION_FOR_TESTING,
                          DEFAULT_OP_COUNT);
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

    // Check if creation source accounts have been created
    for (auto it = mCreationSourceAccounts.begin();
         it != mCreationSourceAccounts.end();)
    {
        if (mTxGenerator.loadAccount(it->second))
        {
            mAccountsAvailable.insert(it->first);
            it = mCreationSourceAccounts.erase(it);
        }
        else
        {
            ++it;
        }
    }

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
void
LoadGenerator::reset()
{
    mTxGenerator.reset();
    mAccountsInUse.clear();
    mAccountsAvailable.clear();
    mCreationSourceAccounts.clear();
    mContractInstances.clear();
    mLoadTimer.reset();
    mRoot.reset();
    mStartTime.reset();
    mTotalSubmitted = 0;
    mWaitTillCompleteForLedgers = 0;
    mSorobanWasmWaitTillLedgers = 0;
    mFailed = false;
    mStarted = false;
    mInitialAccountsCreated = false;
    mPreLoadgenApplySorobanSuccess = 0;
    mPreLoadgenApplySorobanFailure = 0;
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

    createRootAccount();

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

    if (cfg.mode != LoadGenMode::CREATE)
    {
        // Mark all accounts "available" as source accounts
        for (auto i = 0u; i < cfg.nAccounts; i++)
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
    if (cfg.mode != LoadGenMode::CREATE && cfg.nTxs > cfg.nAccounts &&
        (cfg.txRate * Herder::EXP_LEDGER_TIMESPAN_SECONDS.count()) *
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
    return (isCreate() && nAccounts == 0) || (isLoad() && nTxs == 0) ||
           (isSorobanSetup() && getSorobanConfig().nInstances == 0);
}

bool
GeneratedLoadConfig::areTxsRemaining() const
{
    return (isCreate() && nAccounts != 0) || (!isCreate() && nTxs != 0);
}

Json::Value
GeneratedLoadConfig::getStatus() const
{
    Json::Value ret;
    std::string modeStr;
    switch (mode)
    {
    case LoadGenMode::CREATE:
        modeStr = "create";
        break;
    case LoadGenMode::PAY:
        modeStr = "pay";
        break;
    case LoadGenMode::PRETEND:
        modeStr = "pretend";
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
    }

    ret["mode"] = modeStr;

    if (isCreate())
    {
        ret["accounts_remaining"] = nAccounts;
    }
    else if (isSorobanSetup())
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
        if (!cfg.isCreate() && cfg.skipLowFeeTxs)
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
    if (cfg.mode == LoadGenMode::CREATE)
    {
        // Limit creation to the number of accounts we have. This is only the
        // case at the very beginning, when only root account is available for
        // account creation
        size_t expectedSize =
            mInitialAccountsCreated ? mAccountsAvailable.size() : 1;
        txPerStep = std::min<int64_t>(txPerStep, expectedSize);
    }
    auto submitScope = mStepTimer.TimeScope();

    uint64_t now = mApp.timeNow();
    // Cleaning up accounts every second, so we don't call potentially expensive
    // cleanup function too often
    if (now != mLastSecond)
    {
        cleanupAccounts();
    }

    uint32_t ledgerNum = mApp.getLedgerManager().getLastClosedLedgerNum() + 1;

    for (int64_t i = 0; i < txPerStep; ++i)
    {
        if (cfg.mode == LoadGenMode::CREATE)
        {
            cfg.nAccounts =
                submitCreationTx(cfg.nAccounts, cfg.offset, ledgerNum);
        }
        else
        {
            if (mAccountsAvailable.empty())
            {
                CLOG_WARNING(
                    LoadGen,
                    "Load generation failed: no more accounts available");
                mLoadgenFail.Mark();
                reset();
                return;
            }

            uint64_t sourceAccountId = getNextAvailableAccount(ledgerNum);

            std::function<std::pair<TxGenerator::TestAccountPtr,
                                    TransactionFrameBaseConstPtr>()>
                generateTx;

            switch (cfg.mode)
            {
            case LoadGenMode::CREATE:
                releaseAssert(false);
                break;
            case LoadGenMode::PAY:
                generateTx = [&]() {
                    return mTxGenerator.paymentTransaction(
                        cfg.nAccounts, cfg.offset, ledgerNum, sourceAccountId,
                        1, cfg.maxGeneratedFeeRate);
                };
                break;
            case LoadGenMode::PRETEND:
            {
                auto opCount = chooseOpCount(mApp.getConfig());
                generateTx = [&, opCount]() {
                    return mTxGenerator.pretendTransaction(
                        cfg.nAccounts, cfg.offset, ledgerNum, sourceAccountId,
                        opCount, cfg.maxGeneratedFeeRate);
                };
            }
            break;
            case LoadGenMode::MIXED_CLASSIC:
            {
                auto opCount = chooseOpCount(mApp.getConfig());
                bool isDex =
                    rand_uniform<uint32_t>(1, 100) <= cfg.getDexTxPercent();
                generateTx = [&, opCount, isDex]() {
                    if (isDex)
                    {
                        return mTxGenerator.manageOfferTransaction(
                            ledgerNum, sourceAccountId, opCount,
                            cfg.maxGeneratedFeeRate);
                    }
                    else
                    {
                        return mTxGenerator.paymentTransaction(
                            cfg.nAccounts, cfg.offset, ledgerNum,
                            sourceAccountId, opCount, cfg.maxGeneratedFeeRate);
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
                    auto instanceIter =
                        mContractInstances.find(sourceAccountId);
                    releaseAssert(instanceIter != mContractInstances.end());
                    auto const& instance = instanceIter->second;
                    return mTxGenerator.invokeSorobanLoadTransaction(
                        ledgerNum, sourceAccountId, instance,
                        mContactOverheadBytes, cfg.maxGeneratedFeeRate);
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
                        *mContractInstanceKeys.begin(),
                        cfg.maxGeneratedFeeRate);
                };
                break;
            case LoadGenMode::MIXED_CLASSIC_SOROBAN:
                generateTx = [&]() {
                    return createMixedClassicSorobanTransaction(
                        ledgerNum, sourceAccountId, cfg);
                };
                break;
            }

            if (submitTx(cfg, generateTx))
            {
                --cfg.nTxs;
            }
            else if (mFailed)
            {
                break;
            }
        }
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
    mTotalSubmitted += txPerStep;
    scheduleLoadGeneration(cfg);
}

uint32_t
LoadGenerator::submitCreationTx(uint32_t nAccounts, uint32_t offset,
                                uint32_t ledgerNum)
{
    uint32_t numToProcess =
        nAccounts < MAX_OPS_PER_TX ? nAccounts : MAX_OPS_PER_TX;
    auto [from, tx] = creationTransaction(
        mTxGenerator.getAccounts().size() + offset, numToProcess, ledgerNum);
    TransactionResultCode code;
    TransactionQueue::AddResultCode status;
    bool createDuplicate = false;
    uint32_t numTries = 0;

    while ((status = execute(tx, LoadGenMode::CREATE, code)) !=
           TransactionQueue::AddResultCode::ADD_STATUS_PENDING)
    {
        // Ignore duplicate transactions, simply continue generating load
        if (status == TransactionQueue::AddResultCode::ADD_STATUS_DUPLICATE)
        {
            createDuplicate = true;
            break;
        }

        if (++numTries >= TX_SUBMIT_MAX_TRIES ||
            status != TransactionQueue::AddResultCode::ADD_STATUS_ERROR)
        {
            // Failed to submit the step of load
            mFailed = true;
            return 0;
        }

        // In case of bad seqnum, attempt refreshing it from the DB
        maybeHandleFailedTx(tx, from, status, code);
    }

    if (!createDuplicate)
    {
        nAccounts -= numToProcess;
    }

    return nAccounts;
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

        if (cfg.skipLowFeeTxs &&
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
        if (++numTries >= TX_SUBMIT_MAX_TRIES ||
            status != TransactionQueue::AddResultCode::ADD_STATUS_ERROR)
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
    if (cfg.mode == LoadGenMode::CREATE)
    {
        remainingTxCount = cfg.nAccounts / MAX_OPS_PER_TX;
    }
    else if (cfg.isSorobanSetup())
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
    auto etaMins = etaSecs % 60;

    if (cfg.isSoroban())
    {
        CLOG_INFO(LoadGen,
                  "Tx/s: {} target, {} tx actual (1m EWMA). Pending: {} txs. "
                  "ETA: {}h{}m",
                  cfg.txRate, applyTx.one_minute_rate(), remainingTxCount,
                  etaHours, etaMins);
    }
    else if (cfg.mode == LoadGenMode::CREATE)
    {
        CLOG_INFO(LoadGen,
                  "Tx/s: {} target, {}tx/{}op actual (1m EWMA). Pending: {} "
                  "accounts, {} txs. ETA: {}h{}m",
                  cfg.txRate, applyTx.one_minute_rate(),
                  applyOp.one_minute_rate(), cfg.nAccounts, remainingTxCount,
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
LoadGenerator::creationTransaction(uint64_t startAccount, uint64_t numItems,
                                   uint32_t ledgerNum)
{
    TxGenerator::TestAccountPtr sourceAcc =
        mInitialAccountsCreated
            ? mTxGenerator.findAccount(getNextAvailableAccount(ledgerNum),
                                       ledgerNum)
            : mRoot;
    vector<Operation> creationOps = mTxGenerator.createAccounts(
        startAccount, numItems, ledgerNum, !mInitialAccountsCreated);
    if (!mInitialAccountsCreated)
    {
        auto const& initialAccounts = mTxGenerator.getAccounts();
        for (auto const& kvp : initialAccounts)
        {
            mCreationSourceAccounts.emplace(kvp.first, kvp.second);
        }
    }
    mInitialAccountsCreated = true;
    return std::make_pair(sourceAcc,
                          mTxGenerator.createTransactionFramePtr(
                              sourceAcc, creationOps, false, std::nullopt));
}

std::pair<TxGenerator::TestAccountPtr, TransactionFrameBaseConstPtr>
LoadGenerator::createMixedClassicSorobanTransaction(
    uint32_t ledgerNum, uint64_t sourceAccountId,
    GeneratedLoadConfig const& cfg)
{
    auto const& mixCfg = cfg.getMixClassicSorobanConfig();
    std::discrete_distribution<uint32_t> dist({mixCfg.payWeight,
                                               mixCfg.sorobanUploadWeight,
                                               mixCfg.sorobanInvokeWeight});
    switch (dist(gRandomEngine))
    {
    case 0:
    {
        // Create a payment transaction
        mLastMixedMode = LoadGenMode::PAY;
        return mTxGenerator.paymentTransaction(cfg.nAccounts, cfg.offset,
                                               ledgerNum, sourceAccountId, 1,
                                               cfg.maxGeneratedFeeRate);
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
LoadGenerator::checkAccountSynced(Application& app, bool isCreate)
{
    std::vector<TxGenerator::TestAccountPtr> result;
    for (auto const& acc : mTxGenerator.getAccounts())
    {
        TxGenerator::TestAccountPtr account = acc.second;
        auto accountFromDB = *account;

        auto reloadRes = mTxGenerator.loadAccount(accountFromDB);
        // For account creation, reload accounts from the DB
        // For payments, ensure that the sequence number matches expected
        // seqnum. Timeout after 20 ledgers.
        if (isCreate)
        {
            if (!reloadRes)
            {
                CLOG_TRACE(LoadGen, "Account {} is not created yet!",
                           account->getAccountId());
                result.push_back(account);
            }
            else if (app.getHerder().sourceAccountPending(
                         account->getPublicKey()))
            {
                CLOG_TRACE(LoadGen, "Account {} is pending!",
                           account->getAccountId());
                result.push_back(account);
            }
        }
        else if (!reloadRes)
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
    vector<TxGenerator::TestAccountPtr> inconsistencies;
    inconsistencies = checkAccountSynced(mApp, cfg.isCreate());
    auto sorobanInconsistencies = checkSorobanStateSynced(mApp, cfg);

    // If there are no inconsistencies and we have generated all load, finish
    if (inconsistencies.empty() && sorobanInconsistencies.empty() &&
        cfg.isDone())
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
    else if (!inconsistencies.empty() || !sorobanInconsistencies.empty())
    {
        if (++mWaitTillCompleteForLedgers >= TIMEOUT_NUM_LEDGERS)
        {
            emitFailure(!sorobanInconsistencies.empty());
            return;
        }

        mLoadTimer->expires_from_now(
            mApp.getConfig().getExpectedLedgerCloseTime());
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
        auto inconsistencies = checkAccountSynced(mApp, /* isCreate */ false);
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
    mLoadTimer->expires_from_now(mApp.getConfig().getExpectedLedgerCloseTime());
    mLoadTimer->async_wait([this]() { this->waitTillCompleteWithoutChecks(); },
                           &VirtualTimer::onFailureNoop);
}

LoadGenerator::TxMetrics::TxMetrics(medida::MetricsRegistry& m)
    : mAccountCreated(m.NewMeter({"loadgen", "account", "created"}, "account"))
    , mNativePayment(m.NewMeter({"loadgen", "payment", "submitted"}, "op"))
    , mManageOfferOps(m.NewMeter({"loadgen", "manageoffer", "submitted"}, "op"))
    , mPretendOps(m.NewMeter({"loadgen", "pretend", "submitted"}, "op"))
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
               "Counts: {} tx, {} rj, {} by, {} ac, {} na, {} pr, {} dex, {} "
               "su, {} ssi, {} ssu, {} si, {} scu",
               mTxnAttempted.count(), mTxnRejected.count(), mTxnBytes.count(),
               mAccountCreated.count(), mNativePayment.count(),
               mPretendOps.count(), mManageOfferOps.count(),
               mSorobanUploadTxs.count(), mSorobanSetupInvokeTxs.count(),
               mSorobanSetupUpgradeTxs.count(), mSorobanInvokeTxs.count(),
               mSorobanCreateUpgradeTxs.count());

    CLOG_DEBUG(LoadGen,
               "Rates/sec (1m EWMA): {} tx, {} rj, {} by, {} ac, {} na, {} pr, "
               "{} dex, {} su, {} ssi, {} ssu, {} si, {} scu",
               mTxnAttempted.one_minute_rate(), mTxnRejected.one_minute_rate(),
               mTxnBytes.one_minute_rate(), mAccountCreated.one_minute_rate(),
               mNativePayment.one_minute_rate(), mPretendOps.one_minute_rate(),
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
    case LoadGenMode::CREATE:
        txm.mAccountCreated.Mark(txf->getNumOperations());
        break;
    case LoadGenMode::PAY:
        txm.mNativePayment.Mark(txf->getNumOperations());
        break;
    case LoadGenMode::PRETEND:
        txm.mPretendOps.Mark(txf->getNumOperations());
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
    }

    txm.mTxnAttempted.Mark();

    auto msg = txf->toStellarMessage();
    txm.mTxnBytes.Mark(xdr::xdr_argpack_size(*msg));

    auto addResult = mApp.getHerder().recvTransaction(txf, true);
    if (addResult.code != TransactionQueue::AddResultCode::ADD_STATUS_PENDING)
    {

        auto resultStr =
            addResult.txResult
                ? xdrToCerealString(addResult.txResult->getResult(),
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
    SorobanNetworkConfig const& cfg)
{
    releaseAssert(mode == LoadGenMode::SOROBAN_CREATE_UPGRADE);
    auto& upgradeCfg = getMutSorobanUpgradeConfig();

    upgradeCfg.maxContractSizeBytes = cfg.maxContractSizeBytes();
    upgradeCfg.maxContractDataKeySizeBytes = cfg.maxContractDataKeySizeBytes();
    upgradeCfg.maxContractDataEntrySizeBytes =
        cfg.maxContractDataEntrySizeBytes();

    upgradeCfg.ledgerMaxInstructions = cfg.ledgerMaxInstructions();
    upgradeCfg.txMaxInstructions = cfg.txMaxInstructions();
    upgradeCfg.txMemoryLimit = cfg.txMemoryLimit();

    upgradeCfg.ledgerMaxReadLedgerEntries = cfg.ledgerMaxReadLedgerEntries();
    upgradeCfg.ledgerMaxReadBytes = cfg.ledgerMaxReadBytes();
    upgradeCfg.ledgerMaxWriteLedgerEntries = cfg.ledgerMaxWriteLedgerEntries();
    upgradeCfg.ledgerMaxWriteBytes = cfg.ledgerMaxWriteBytes();
    upgradeCfg.ledgerMaxTxCount = cfg.ledgerMaxTxCount();
    upgradeCfg.txMaxReadLedgerEntries = cfg.txMaxReadLedgerEntries();
    upgradeCfg.txMaxReadBytes = cfg.txMaxReadBytes();
    upgradeCfg.txMaxWriteLedgerEntries = cfg.txMaxWriteLedgerEntries();
    upgradeCfg.txMaxWriteBytes = cfg.txMaxWriteBytes();

    upgradeCfg.txMaxContractEventsSizeBytes =
        cfg.txMaxContractEventsSizeBytes();

    upgradeCfg.ledgerMaxTransactionsSizeBytes =
        cfg.ledgerMaxTransactionSizesBytes();
    upgradeCfg.txMaxSizeBytes = cfg.txMaxSizeBytes();

    upgradeCfg.maxEntryTTL = cfg.stateArchivalSettings().maxEntryTTL;
    upgradeCfg.minTemporaryTTL = cfg.stateArchivalSettings().minTemporaryTTL;
    upgradeCfg.minPersistentTTL = cfg.stateArchivalSettings().minPersistentTTL;
    upgradeCfg.persistentRentRateDenominator =
        cfg.stateArchivalSettings().persistentRentRateDenominator;
    upgradeCfg.tempRentRateDenominator =
        cfg.stateArchivalSettings().tempRentRateDenominator;
    upgradeCfg.maxEntriesToArchive =
        cfg.stateArchivalSettings().maxEntriesToArchive;
    upgradeCfg.bucketListSizeWindowSampleSize =
        cfg.stateArchivalSettings().bucketListSizeWindowSampleSize;
    upgradeCfg.bucketListWindowSamplePeriod =
        cfg.stateArchivalSettings().bucketListWindowSamplePeriod;
    upgradeCfg.evictionScanSize = cfg.stateArchivalSettings().evictionScanSize;
    upgradeCfg.startingEvictionScanLevel =
        cfg.stateArchivalSettings().startingEvictionScanLevel;
}

GeneratedLoadConfig
GeneratedLoadConfig::createAccountsLoad(uint32_t nAccounts, uint32_t txRate)
{
    GeneratedLoadConfig cfg;
    cfg.mode = LoadGenMode::CREATE;
    cfg.nAccounts = nAccounts;
    cfg.txRate = txRate;
    return cfg;
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
GeneratedLoadConfig::isCreate() const
{
    return mode == LoadGenMode::CREATE;
}

bool
GeneratedLoadConfig::isSoroban() const
{
    return mode == LoadGenMode::SOROBAN_INVOKE ||
           mode == LoadGenMode::SOROBAN_INVOKE_SETUP ||
           mode == LoadGenMode::SOROBAN_UPLOAD ||
           mode == LoadGenMode::SOROBAN_UPGRADE_SETUP ||
           mode == LoadGenMode::SOROBAN_CREATE_UPGRADE ||
           mode == LoadGenMode::MIXED_CLASSIC_SOROBAN;
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
    return mode == LoadGenMode::PAY || mode == LoadGenMode::PRETEND ||
           mode == LoadGenMode::MIXED_CLASSIC ||
           mode == LoadGenMode::SOROBAN_UPLOAD ||
           mode == LoadGenMode::SOROBAN_INVOKE ||
           mode == LoadGenMode::SOROBAN_CREATE_UPGRADE ||
           mode == LoadGenMode::MIXED_CLASSIC_SOROBAN;
}

bool
GeneratedLoadConfig::modeInvokes() const
{
    return mode == LoadGenMode::SOROBAN_INVOKE ||
           mode == LoadGenMode::MIXED_CLASSIC_SOROBAN;
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
}
