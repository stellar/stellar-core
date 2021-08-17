// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "simulation/LoadGenerator.h"
#include "herder/Herder.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "main/Config.h"
#include "overlay/OverlayManager.h"
#include "test/TestAccount.h"
#include "test/TxTests.h"
#include "transactions/TransactionBridge.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/Timer.h"
#include "util/XDRCereal.h"
#include "util/numeric.h"
#include "util/types.h"

#include "database/Database.h"

#include "xdrpp/marshal.h"

#include "medida/meter.h"
#include "medida/metrics_registry.h"

#include <cmath>
#include <fmt/format.h>
#include <iomanip>
#include <set>

namespace stellar
{

using namespace std;
using namespace txtest;

// Units of load are scheduled at 100ms intervals.
const uint32_t LoadGenerator::STEP_MSECS = 100;

// If submission fails with txBAD_SEQ, attempt refreshing the account or
// re-submitting a new payment
const uint32_t LoadGenerator::TX_SUBMIT_MAX_TRIES = 10;

// After successfully submitting desired load, wait a bit to let it get into the
// ledger.
const uint32_t LoadGenerator::TIMEOUT_NUM_LEDGERS = 20;

LoadGenerator::LoadGenerator(Application& app)
    : mMinBalance(0)
    , mLastSecond(0)
    , mApp(app)
    , mTotalSubmitted(0)
    , mLoadgenComplete(
          mApp.getMetrics().NewMeter({"loadgen", "run", "complete"}, "run"))
    , mLoadgenFail(
          mApp.getMetrics().NewMeter({"loadgen", "run", "failed"}, "run"))
{
    createRootAccount();
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
    else
    {
        // unknown mode
        abort();
    }
}

void
LoadGenerator::createRootAccount()
{
    if (!mRoot)
    {
        auto rootTestAccount = TestAccount::createRoot(mApp);
        mRoot = make_shared<TestAccount>(rootTestAccount);
        if (!loadAccount(mRoot, mApp))
        {
            CLOG_ERROR(LoadGen, "Could not retrieve root account!");
        }
    }
}

unsigned short
LoadGenerator::chooseOpCount(Config const& cfg) const
{
    if (cfg.LOADGEN_OP_COUNT_FOR_TESTING.empty())
    {
        return 1;
    }
    else
    {
        std::discrete_distribution<uint32> distribution(
            cfg.LOADGEN_OP_COUNT_DISTRIBUTION_FOR_TESTING.begin(),
            cfg.LOADGEN_OP_COUNT_DISTRIBUTION_FOR_TESTING.end());
        return cfg.LOADGEN_OP_COUNT_FOR_TESTING[distribution(gRandomEngine)];
    }
}

int64_t
LoadGenerator::getTxPerStep(uint32_t txRate, std::chrono::seconds spikeInterval,
                            uint32_t spikeSize)
{
    if (!mStartTime)
    {
        throw std::runtime_error("Load generation start time must be set");
    }

    auto& stepMeter =
        mApp.getMetrics().NewMeter({"loadgen", "step", "count"}, "step");
    stepMeter.Mark();

    auto now = mApp.getClock().now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - *mStartTime);
    auto txs = bigDivide(elapsed.count(), txRate, 1000, Rounding::ROUND_DOWN);
    if (spikeInterval.count() > 0)
    {
        txs +=
            bigDivide(std::chrono::duration_cast<std::chrono::seconds>(elapsed)
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
LoadGenerator::reset()
{
    mAccounts.clear();
    mRoot.reset();
    mStartTime.reset();
    mTotalSubmitted = 0;
    mWaitTillCompleteForLedgers = 0;
    mFailed = false;
}

// Schedule a callback to generateLoad() STEP_MSECS milliseconds from now.
void
LoadGenerator::scheduleLoadGeneration(LoadGenMode mode, uint32_t nAccounts,
                                      uint32_t offset, uint32_t nTxs,
                                      uint32_t txRate, uint32_t batchSize,
                                      std::chrono::seconds spikeInterval,
                                      uint32_t spikeSize)
{
    // If previously scheduled step of load did not succeed, fail this loadgen
    // run.
    if (mFailed)
    {
        CLOG_ERROR(LoadGen, "Load generation failed, ensure correct "
                            "number parameters are set and accounts are "
                            "created, or retry with smaller tx rate.");
        mLoadgenFail.Mark();
        reset();
        return;
    }

    if (!mLoadTimer)
    {
        mLoadTimer = std::make_unique<VirtualTimer>(mApp.getClock());
    }

    if (mApp.getState() == Application::APP_SYNCED_STATE)
    {
        mLoadTimer->expires_from_now(std::chrono::milliseconds(STEP_MSECS));
        mLoadTimer->async_wait(
            [this, nAccounts, offset, nTxs, txRate, batchSize, mode,
             spikeInterval, spikeSize]() {
                this->generateLoad(mode, nAccounts, offset, nTxs, txRate,
                                   batchSize, spikeInterval, spikeSize);
            },
            &VirtualTimer::onFailureNoop);
    }
    else
    {
        CLOG_WARNING(
            LoadGen,
            "Application is not in sync, load generation inhibited. State {}",
            mApp.getState());
        mLoadTimer->expires_from_now(std::chrono::seconds(10));
        mLoadTimer->async_wait(
            [this, nAccounts, offset, nTxs, txRate, batchSize, mode,
             spikeInterval, spikeSize]() {
                this->scheduleLoadGeneration(mode, nAccounts, offset, nTxs,
                                             txRate, batchSize, spikeInterval,
                                             spikeSize);
            },
            &VirtualTimer::onFailureNoop);
    }
}

// Generate one "step" worth of load (assuming 1 step per STEP_MSECS) at a
// given target number of accounts and txs, and a given target tx/s rate.
// If work remains after the current step, call scheduleLoadGeneration()
// with the remainder.
void
LoadGenerator::generateLoad(LoadGenMode mode, uint32_t nAccounts,
                            uint32_t offset, uint32_t nTxs, uint32_t txRate,
                            uint32_t batchSize,
                            std::chrono::seconds spikeInterval,
                            uint32_t spikeSize)

{
    bool isCreate = mode == LoadGenMode::CREATE;
    if (!mStartTime)
    {
        mStartTime =
            std::make_unique<VirtualClock::time_point>(mApp.getClock().now());
    }

    createRootAccount();

    // Finish if no more txs need to be created.
    if ((isCreate && nAccounts == 0) || (!isCreate && nTxs == 0))
    {
        // Done submitting the load, now ensure it propagates to the DB.
        waitTillComplete(isCreate);
        return;
    }

    updateMinBalance();
    if (txRate == 0)
    {
        txRate = 1;
    }
    if (batchSize == 0)
    {
        batchSize = 1;
    }

    auto txPerStep = getTxPerStep(txRate, spikeInterval, spikeSize);
    auto& submitTimer =
        mApp.getMetrics().NewTimer({"loadgen", "step", "submit"});
    auto submitScope = submitTimer.TimeScope();

    uint32_t ledgerNum = mApp.getLedgerManager().getLastClosedLedgerNum() + 1;

    for (int64_t i = 0; i < txPerStep; ++i)
    {
        switch (mode)
        {
        case LoadGenMode::CREATE:
            nAccounts =
                submitCreationTx(nAccounts, offset, batchSize, ledgerNum);
            break;
        case LoadGenMode::PAY:
            nTxs = submitPaymentOrPretendTx(nAccounts, offset, batchSize,
                                            ledgerNum, nTxs, 1, mode);
            break;
        case LoadGenMode::PRETEND:
            auto opCount = chooseOpCount(mApp.getConfig());
            nTxs = submitPaymentOrPretendTx(nAccounts, offset, batchSize,
                                            ledgerNum, nTxs, opCount, mode);
            break;
        }

        if (nAccounts == 0 || (!isCreate && nTxs == 0))
        {
            // Nothing to do for the rest of the step
            break;
        }
    }

    auto submit = submitScope.Stop();

    uint64_t now = mApp.timeNow();

    // Emit a log message once per second.
    if (now != mLastSecond)
    {
        logProgress(submit, mode, nAccounts, nTxs, batchSize, txRate);
    }

    mLastSecond = now;
    mTotalSubmitted += txPerStep;
    scheduleLoadGeneration(mode, nAccounts, offset, nTxs, txRate, batchSize,
                           spikeInterval, spikeSize);
}

uint32_t
LoadGenerator::submitCreationTx(uint32_t nAccounts, uint32_t offset,
                                uint32_t batchSize, uint32_t ledgerNum)
{
    uint32_t numToProcess = nAccounts < batchSize ? nAccounts : batchSize;
    TestAccountPtr from;
    TransactionFramePtr tx;
    std::tie(from, tx) =
        creationTransaction(mAccounts.size() + offset, numToProcess, ledgerNum);
    TransactionResultCode code;
    TransactionQueue::AddResult status;
    bool createDuplicate = false;
    uint32_t numTries = 0;

    while ((status = execute(tx, LoadGenMode::CREATE, code, batchSize)) !=
           TransactionQueue::AddResult::ADD_STATUS_PENDING)
    {
        // Ignore duplicate transactions, simply continue generating load
        if (status == TransactionQueue::AddResult::ADD_STATUS_DUPLICATE)
        {
            createDuplicate = true;
            break;
        }

        if (++numTries >= TX_SUBMIT_MAX_TRIES ||
            status != TransactionQueue::AddResult::ADD_STATUS_ERROR)
        {
            // Failed to submit the step of load
            mFailed = true;
            return 0;
        }

        // In case of bad seqnum, attempt refreshing it from the DB
        maybeHandleFailedTx(from, status, code);
    }

    if (!createDuplicate)
    {
        nAccounts -= numToProcess;
    }

    return nAccounts;
}

uint32_t
LoadGenerator::submitPaymentOrPretendTx(uint32_t nAccounts, uint32_t offset,
                                        uint32_t batchSize, uint32_t ledgerNum,
                                        uint32_t nTxs, uint32_t opCount,
                                        LoadGenMode mode)
{
    auto sourceAccountId = rand_uniform<uint64_t>(0, nAccounts - 1) + offset;
    TransactionFramePtr tx;
    TestAccountPtr from;
    bool usePaymentOp = mode == LoadGenMode::PAY;
    std::tie(from, tx) =
        usePaymentOp
            ? paymentTransaction(nAccounts, offset, ledgerNum, sourceAccountId)
            : pretendTransaction(nAccounts, offset, ledgerNum, sourceAccountId,
                                 opCount);

    TransactionResultCode code;
    TransactionQueue::AddResult status;
    uint32_t numTries = 0;

    while ((status = execute(tx, mode, code, batchSize)) !=
           TransactionQueue::AddResult::ADD_STATUS_PENDING)
    {
        if (++numTries >= TX_SUBMIT_MAX_TRIES ||
            status != TransactionQueue::AddResult::ADD_STATUS_ERROR)
        {
            mFailed = true;
            return 0;
        }

        // In case of bad seqnum, attempt refreshing it from the DB
        maybeHandleFailedTx(from, status, code); // Update seq num

        // Regenerate a new payment tx
        std::tie(from, tx) =
            usePaymentOp ? paymentTransaction(nAccounts, offset, ledgerNum,
                                              sourceAccountId)
                         : pretendTransaction(nAccounts, offset, ledgerNum,
                                              sourceAccountId, opCount);
    }

    nTxs -= 1;
    return nTxs;
}

void
LoadGenerator::logProgress(std::chrono::nanoseconds submitTimer,
                           LoadGenMode mode, uint32_t nAccounts, uint32_t nTxs,
                           uint32_t batchSize, uint32_t txRate)
{
    using namespace std::chrono;

    auto& m = mApp.getMetrics();
    auto& applyTx = m.NewTimer({"ledger", "transaction", "apply"});
    auto& applyOp = m.NewTimer({"ledger", "operation", "apply"});

    auto submitSteps = duration_cast<milliseconds>(submitTimer).count();

    auto remainingTxCount =
        (mode == LoadGenMode::CREATE) ? nAccounts / batchSize : nTxs;
    auto etaSecs = (uint32_t)(((double)remainingTxCount) /
                              max<double>(1, applyTx.one_minute_rate()));

    auto etaHours = etaSecs / 3600;
    auto etaMins = etaSecs % 60;

    CLOG_INFO(LoadGen,
              "Tx/s: {} target, {}tx/{}op actual (1m EWMA). Pending: {} "
              "accounts, {} txs. ETA: {}h{}m",
              txRate, applyTx.one_minute_rate(), applyOp.one_minute_rate(),
              nAccounts, nTxs, etaHours, etaMins);

    CLOG_DEBUG(LoadGen, "Step timing: {}ms submit.", submitSteps);

    TxMetrics txm(mApp.getMetrics());
    txm.report();
}

std::pair<LoadGenerator::TestAccountPtr, TransactionFramePtr>
LoadGenerator::creationTransaction(uint64_t startAccount, uint64_t numItems,
                                   uint32_t ledgerNum)
{
    vector<Operation> creationOps =
        createAccounts(startAccount, numItems, ledgerNum);
    return std::make_pair(mRoot, createTransactionFramePtr(
                                     mRoot, creationOps, LoadGenMode::CREATE));
}

void
LoadGenerator::updateMinBalance()
{
    auto b = mApp.getLedgerManager().getLastMinBalance(0);
    if (b > mMinBalance)
    {
        mMinBalance = b;
    }
}

std::vector<Operation>
LoadGenerator::createAccounts(uint64_t start, uint64_t count,
                              uint32_t ledgerNum)
{
    vector<Operation> ops;
    SequenceNumber sn = static_cast<SequenceNumber>(ledgerNum) << 32;
    for (uint64_t i = start; i < start + count; i++)
    {
        auto name = "TestAccount-" + to_string(i);
        auto account = TestAccount{mApp, txtest::getAccount(name.c_str()), sn};
        ops.push_back(
            txtest::createAccount(account.getPublicKey(), mMinBalance * 100));

        // Cache newly created account
        mAccounts.insert(std::pair<uint64_t, TestAccountPtr>(
            i, make_shared<TestAccount>(account)));
    }
    return ops;
}

bool
LoadGenerator::loadAccount(TestAccount& account, Application& app)
{
    LedgerTxn ltx(app.getLedgerTxnRoot());
    auto entry = stellar::loadAccount(ltx, account.getPublicKey());
    if (!entry)
    {
        return false;
    }
    account.setSequenceNumber(entry.current().data.account().seqNum);
    return true;
}

bool
LoadGenerator::loadAccount(TestAccountPtr acc, Application& app)
{
    if (acc)
    {
        return loadAccount(*acc, app);
    }
    return false;
}

std::pair<LoadGenerator::TestAccountPtr, LoadGenerator::TestAccountPtr>
LoadGenerator::pickAccountPair(uint32_t numAccounts, uint32_t offset,
                               uint32_t ledgerNum, uint64_t sourceAccountId)
{
    auto sourceAccount = findAccount(sourceAccountId, ledgerNum);

    // Mod with total number of accounts to ensure account exists
    uint64_t destAccountId =
        (sourceAccountId + sourceAccount->getLastSequenceNumber()) %
            numAccounts +
        offset;

    auto destAccount = findAccount(destAccountId, ledgerNum);

    CLOG_DEBUG(LoadGen, "Generated pair for payment tx - {} and {}",
               sourceAccountId, destAccountId);
    return std::pair<TestAccountPtr, TestAccountPtr>(sourceAccount,
                                                     destAccount);
}

LoadGenerator::TestAccountPtr
LoadGenerator::findAccount(uint64_t accountId, uint32_t ledgerNum)
{
    // Load account and cache it.
    TestAccountPtr newAccountPtr;

    auto res = mAccounts.find(accountId);
    if (res == mAccounts.end())
    {
        SequenceNumber sn = static_cast<SequenceNumber>(ledgerNum) << 32;
        auto name = "TestAccount-" + std::to_string(accountId);
        newAccountPtr =
            std::make_shared<TestAccount>(mApp, txtest::getAccount(name), sn);

        if (!loadAccount(newAccountPtr, mApp))
        {
            throw std::runtime_error(
                fmt::format("Account {0} must exist in the DB.", accountId));
        }
        mAccounts.insert(
            std::pair<uint64_t, TestAccountPtr>(accountId, newAccountPtr));
    }
    else
    {
        newAccountPtr = res->second;
    }

    return newAccountPtr;
}

std::pair<LoadGenerator::TestAccountPtr, TransactionFramePtr>
LoadGenerator::paymentTransaction(uint32_t numAccounts, uint32_t offset,
                                  uint32_t ledgerNum, uint64_t sourceAccount)
{
    TestAccountPtr to, from;
    uint64_t amount = 1;
    std::tie(from, to) =
        pickAccountPair(numAccounts, offset, ledgerNum, sourceAccount);
    vector<Operation> paymentOps = {
        txtest::payment(to->getPublicKey(), amount)};
    return std::make_pair(
        from, createTransactionFramePtr(from, paymentOps, LoadGenMode::PAY));
}

std::pair<LoadGenerator::TestAccountPtr, TransactionFramePtr>
LoadGenerator::pretendTransaction(uint32_t numAccounts, uint32_t offset,
                                  uint32_t ledgerNum, uint64_t sourceAccount,
                                  uint32_t opCount)
{
    vector<Operation> ops;
    ops.reserve(opCount);
    auto acc = findAccount(sourceAccount, ledgerNum);
    for (uint32 i = 0; i < opCount; i++)
    {
        auto args = SetOptionsArguments{};

        // We make SetOptionsOps such that we end up
        // with a n-op transaction that is exactly 100n + 240 bytes.
        args.inflationDest = std::make_optional<AccountID>(acc->getPublicKey());
        args.homeDomain = std::make_optional<std::string>(std::string(16, '*'));
        if (i == 0)
        {
            // The first operation needs to be bigger to achieve
            // 100n + 240 bytes.
            args.homeDomain->append(std::string(8, '*'));
            args.signer = std::make_optional<Signer>(Signer{});
        }
        ops.push_back(txtest::setOptions(args));
    }
    return std::make_pair(
        acc, createTransactionFramePtr(acc, ops, LoadGenMode::PRETEND));
}

void
LoadGenerator::maybeHandleFailedTx(TestAccountPtr sourceAccount,
                                   TransactionQueue::AddResult status,
                                   TransactionResultCode code)
{
    // Note that if transaction is a DUPLICATE, its sequence number is
    // incremented on the next call to execute.
    if (status == TransactionQueue::AddResult::ADD_STATUS_ERROR &&
        code == txBAD_SEQ)
    {
        if (!loadAccount(sourceAccount, mApp))
        {
            CLOG_ERROR(LoadGen, "Unable to reload account {}",
                       sourceAccount->getAccountId());
        }
    }
}

std::vector<LoadGenerator::TestAccountPtr>
LoadGenerator::checkAccountSynced(Application& app, bool isCreate)
{
    std::vector<TestAccountPtr> result;
    for (auto const& acc : mAccounts)
    {
        TestAccountPtr account = acc.second;
        auto accountFromDB = *account;

        auto reloadRes = loadAccount(accountFromDB, app);
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

void
LoadGenerator::waitTillComplete(bool isCreate)
{
    if (!mLoadTimer)
    {
        mLoadTimer = std::make_unique<VirtualTimer>(mApp.getClock());
    }
    vector<TestAccountPtr> inconsistencies;
    inconsistencies = checkAccountSynced(mApp, isCreate);

    if (inconsistencies.empty())
    {
        CLOG_INFO(LoadGen, "Load generation complete.");
        mLoadgenComplete.Mark();
        reset();
        return;
    }
    else
    {
        if (++mWaitTillCompleteForLedgers >= TIMEOUT_NUM_LEDGERS)
        {
            CLOG_INFO(LoadGen, "Load generation failed.");
            mLoadgenFail.Mark();
            reset();
            return;
        }

        mLoadTimer->expires_from_now(
            mApp.getConfig().getExpectedLedgerCloseTime());
        mLoadTimer->async_wait(
            [this, isCreate]() { this->waitTillComplete(isCreate); },
            &VirtualTimer::onFailureNoop);
    }
}

LoadGenerator::TxMetrics::TxMetrics(medida::MetricsRegistry& m)
    : mAccountCreated(m.NewMeter({"loadgen", "account", "created"}, "account"))
    , mNativePayment(m.NewMeter({"loadgen", "payment", "native"}, "payment"))
    , mPretendOps(m.NewMeter({"loadgen", "pretend", "submitted"}, "op"))
    , mTxnAttempted(m.NewMeter({"loadgen", "txn", "attempted"}, "txn"))
    , mTxnRejected(m.NewMeter({"loadgen", "txn", "rejected"}, "txn"))
    , mTxnBytes(m.NewMeter({"loadgen", "txn", "bytes"}, "txn"))
{
}

void
LoadGenerator::TxMetrics::report()
{
    CLOG_DEBUG(LoadGen, "Counts: {} tx, {} rj, {} by, {} ac ({} na, {} pr, ",
               mTxnAttempted.count(), mTxnRejected.count(), mTxnBytes.count(),
               mAccountCreated.count(), mNativePayment.count(),
               mPretendOps.count());

    CLOG_DEBUG(
        LoadGen,
        "Rates/sec (1m EWMA): {} tx, {} rj, {} by, {} ac, {} na, {} pr, ",
        mTxnAttempted.one_minute_rate(), mTxnRejected.one_minute_rate(),
        mTxnBytes.one_minute_rate(), mAccountCreated.one_minute_rate(),
        mNativePayment.one_minute_rate(), mPretendOps.one_minute_rate());
}

TransactionFramePtr
LoadGenerator::createTransactionFramePtr(TestAccountPtr from,
                                         std::vector<Operation> ops,
                                         LoadGenMode mode)
{
    auto txf = from->tx(ops);

    if (mode == LoadGenMode::PRETEND)
    {
        Memo memo(MEMO_TEXT);
        memo.text() = std::string(28, ' ');
        txbridge::setMemo(txf, memo);

        txbridge::setMinTime(txf, 0);
        txbridge::setMaxTime(txf, UINT64_MAX);
    }

    txbridge::getSignatures(txf).clear();
    txf->addSignature(from->getSecretKey());
    return txf;
}

TransactionQueue::AddResult
LoadGenerator::execute(TransactionFramePtr& txf, LoadGenMode mode,
                       TransactionResultCode& code, int32_t batchSize)
{
    TxMetrics txm(mApp.getMetrics());

    // Record tx metrics.
    switch (mode)
    {
    case LoadGenMode::CREATE:
        txm.mAccountCreated.Mark(batchSize);
        break;
    case LoadGenMode::PAY:
        txm.mNativePayment.Mark(txf->getNumOperations());
        break;
    case LoadGenMode::PRETEND:
        txm.mPretendOps.Mark(txf->getNumOperations());
    }

    txm.mTxnAttempted.Mark();

    StellarMessage msg(txf->toStellarMessage());
    txm.mTxnBytes.Mark(xdr::xdr_argpack_size(msg));

    auto status = mApp.getHerder().recvTransaction(txf);
    if (status != TransactionQueue::AddResult::ADD_STATUS_PENDING)
    {
        CLOG_INFO(LoadGen, "tx rejected '{}': {} ===> {}",
                  TX_STATUS_STRING[static_cast<int>(status)],
                  xdr_to_string(txf->getEnvelope(), "TransactionEnvelope"),
                  xdr_to_string(txf->getResult(), "TransactionResult"));
        if (status == TransactionQueue::AddResult::ADD_STATUS_ERROR)
        {
            code = txf->getResultCode();
        }
        txm.mTxnRejected.Mark();
    }
    else
    {
        mApp.getOverlayManager().broadcastMessage(msg);
    }

    return status;
}
}
