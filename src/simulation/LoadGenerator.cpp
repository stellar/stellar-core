// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "simulation/LoadGenerator.h"
#include "herder/Herder.h"
#include "ledger/LedgerDelta.h"
#include "ledger/LedgerManager.h"
#include "main/Config.h"
#include "overlay/OverlayManager.h"
#include "test/TestAccount.h"
#include "test/TxTests.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/Timer.h"
#include "util/make_unique.h"
#include "util/types.h"

#include "database/Database.h"

#include "transactions/AllowTrustOpFrame.h"
#include "transactions/ChangeTrustOpFrame.h"
#include "transactions/CreateAccountOpFrame.h"
#include "transactions/ManageOfferOpFrame.h"
#include "transactions/PathPaymentOpFrame.h"
#include "transactions/PaymentOpFrame.h"
#include "transactions/TransactionFrame.h"

#include "xdrpp/marshal.h"
#include "xdrpp/printer.h"

#include "medida/meter.h"
#include "medida/metrics_registry.h"

#include <cmath>
#include <iomanip>
#include <set>

namespace stellar
{

using namespace std;
using namespace txtest;

// Units of load are is scheduled at 100ms intervals.
const uint32_t LoadGenerator::STEP_MSECS = 100;
//
const uint32_t LoadGenerator::TX_SUBMIT_MAX_TRIES = 1000;

LoadGenerator::LoadGenerator(Application& app)
    : mMinBalance(0), mLastSecond(0), mApp(app)
{
    createRootAccount();
}

LoadGenerator::~LoadGenerator()
{
    clear();
}

void
LoadGenerator::createRootAccount()
{
    if (!mRoot)
    {
        auto rootTestAccount = TestAccount::createRoot(mApp);
        mRoot = make_shared<TestAccount>(rootTestAccount);
        auto res = loadAccount(mRoot, mApp.getDatabase());
        if (!res)
        {
            CLOG(ERROR, "LoadGen") << "Could not retrieve root account!";
        }
    }
}

uint32_t
LoadGenerator::getTxPerStep(uint32_t txRate)
{
    // txRate is "per second"; we're running one "step" worth which is a
    // fraction of txRate determined by STEP_MSECS. For example if txRate
    // is 200 and STEP_MSECS is 100, then we want to do 20 tx per step.
    uint32_t txPerStep = (txRate * STEP_MSECS / 1000);

    // There is a wrinkle here though which is that the tx-apply phase might
    // well block timers for up to half the close-time; plus we'll be probably
    // not be scheduled quite as often as we want due to the time it takes to
    // run and the time the network is exchanging packets. So instead of a naive
    // calculation based _just_ on target rate and STEP_MSECS, we also adjust
    // based on how often we seem to be waking up and taking loadgen steps in
    // reality.
    auto& stepMeter =
        mApp.getMetrics().NewMeter({"loadgen", "step", "count"}, "step");
    stepMeter.Mark();
    auto stepsPerSecond = stepMeter.one_minute_rate();
    if (stepMeter.count() > 10 && stepsPerSecond != 0)
    {
        txPerStep = static_cast<uint32_t>(txRate / stepsPerSecond);
    }

    // If we have a very low tx rate (eg. 2/sec) then the previous division will
    // be zero and we'll never issue anything; what we need to do instead is
    // dispatch 1 tx every "few steps" (eg. every 5 steps). We do this by random
    // choice, weighted to the desired frequency.
    if (txPerStep == 0)
    {
        txPerStep = rand_uniform(0U, 1000U) < (txRate * STEP_MSECS) ? 1 : 0;
    }

    return txPerStep;
}

bool
LoadGenerator::maybeAdjustRate(double target, double actual, uint32_t& rate,
                               bool increaseOk)
{
    if (actual == 0.0)
    {
        actual = 1.0;
    }
    double diff = target - actual;
    double acceptableDeviation = 0.1 * target;
    if (fabs(diff) > acceptableDeviation)
    {
        // Limit To doubling rate per adjustment period; even if it's measured
        // as having more room to accelerate, it's likely we'll get a better
        // measurement next time around, and we don't want to overshoot and
        // thrash. Measurement is pretty noisy.
        double pct = std::min(1.0, diff / actual);
        auto incr = static_cast<int32_t>(pct * rate);
        if (incr > 0 && !increaseOk)
        {
            return false;
        }
        CLOG(INFO, "LoadGen")
            << (incr > 0 ? "+++ Increasing" : "--- Decreasing")
            << " auto-tx target rate from " << rate << " to " << rate + incr;
        rate += incr;
        return true;
    }
    return false;
}

void
LoadGenerator::clear()
{
    mAccounts.clear();
    mRoot.reset();
}

// Schedule a callback to generateLoad() STEP_MSECS miliseconds from now.
void
LoadGenerator::scheduleLoadGeneration(bool isCreate, uint32_t nAccounts,
                                      uint32_t nTxs, uint32_t txRate,
                                      uint32_t batchSize, bool autoRate)
{
    if (!mLoadTimer)
    {
        mLoadTimer = make_unique<VirtualTimer>(mApp.getClock());
    }

    if (mApp.getState() == Application::APP_SYNCED_STATE)
    {
        mLoadTimer->expires_from_now(std::chrono::milliseconds(STEP_MSECS));
        mLoadTimer->async_wait([this, nAccounts, nTxs, txRate, batchSize,
                                isCreate,
                                autoRate](asio::error_code const& error) {
            if (!error)
            {
                this->generateLoad(isCreate, nAccounts, nTxs, txRate, batchSize,
                                   autoRate);
            }
        });
    }
    else
    {
        CLOG(WARNING, "LoadGen")
            << "Application is not in sync, load generation inhibited. State "
            << mApp.getState();
        mLoadTimer->expires_from_now(std::chrono::seconds(10));
        mLoadTimer->async_wait([this, nAccounts, nTxs, txRate, batchSize,
                                isCreate,
                                autoRate](asio::error_code const& error) {
            if (!error)
            {
                this->scheduleLoadGeneration(isCreate, nAccounts, nTxs, txRate,
                                             batchSize, autoRate);
            }
        });
    }
}

// Generate one "step" worth of load (assuming 1 step per STEP_MSECS) at a
// given target number of accounts and txs, and a given target tx/s rate.
// If work remains after the current step, call scheduleLoadGeneration()
// with the remainder.
void
LoadGenerator::generateLoad(bool isCreate, uint32_t nAccounts, uint32_t nTxs,
                            uint32_t txRate, uint32_t batchSize, bool autoRate)
{
    soci::transaction sqltx(mApp.getDatabase().getSession());
    mApp.getDatabase().setCurrentTransactionReadOnly();
    createRootAccount();

    // Finish if no more txs need to be created.
    if ((isCreate && nAccounts == 0) || (!isCreate && nTxs == 0))
    {
        // Done submitting the load, now ensure it propagates to the DB.
        waitTillComplete();
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

    uint32_t txPerStep = getTxPerStep(txRate);
    auto& submitTimer =
        mApp.getMetrics().NewTimer({"loadgen", "step", "submit"});
    auto submitScope = submitTimer.TimeScope();

    uint32_t ledgerNum = mApp.getLedgerManager().getLedgerNum();

    for (uint32_t i = 0; i < txPerStep; ++i)
    {
        if (isCreate)
        {
            nAccounts = submitCreationTx(nAccounts, batchSize, ledgerNum);
        }
        else
        {
            nTxs = submitPaymentTx(nAccounts, nTxs, batchSize, ledgerNum);
        }

        if (nAccounts == 0 || (!isCreate && nTxs == 0))
        {
            // Nothing to do for the rest of the step
            break;
        }
    }

    auto submit = submitScope.Stop();

    uint64_t now =
        static_cast<uint64_t>(VirtualClock::to_time_t(mApp.getClock().now()));
    bool secondBoundary = now != mLastSecond;

    if (autoRate && secondBoundary)
    {
        mLastSecond = now;
        inspectRate(ledgerNum, txRate);
    }

    // Emit a log message once per second.
    if (secondBoundary)
    {
        logProgress(submit, isCreate, nAccounts, nTxs, batchSize, txRate);
    }

    scheduleLoadGeneration(isCreate, nAccounts, nTxs, txRate, batchSize,
                           autoRate);
}

uint32_t
LoadGenerator::submitCreationTx(uint32_t nAccounts, uint32_t batchSize,
                                uint32_t ledgerNum)
{
    uint32_t numToProcess = nAccounts < batchSize ? nAccounts : batchSize;
    TxInfo tx = creationTransaction(mAccounts.size(), numToProcess, ledgerNum);
    TransactionResultCode code;
    Herder::TransactionSubmitStatus status;
    bool createDuplicate = false;
    int numTries = 0;

    while ((status = tx.execute(mApp, true, code, batchSize)) !=
           Herder::TX_STATUS_PENDING)
    {
        handleFailedSubmission(tx.mFrom, status, code); // Update seq num
        if (status == Herder::TX_STATUS_DUPLICATE)
        {
            createDuplicate = true;
            break;
        }
        if (++numTries >= TX_SUBMIT_MAX_TRIES)
        {
            CLOG(ERROR, "LoadGen") << "Error creating account!";
            clear();
            return 0;
        }
    }

    if (!createDuplicate)
    {
        nAccounts -= numToProcess;
    }

    return nAccounts;
}

uint32_t
LoadGenerator::submitPaymentTx(uint32_t nAccounts, uint32_t nTxs,
                               uint32_t batchSize, uint32_t ledgerNum)
{
    auto sourceAccountId = rand_uniform<uint64_t>(0, nAccounts - 1);
    TxInfo tx = paymentTransaction(nAccounts, ledgerNum, sourceAccountId);

    TransactionResultCode code;
    Herder::TransactionSubmitStatus status;
    int numTries = 0;

    while ((status = tx.execute(mApp, false, code, batchSize)) !=
           Herder::TX_STATUS_PENDING)
    {
        handleFailedSubmission(tx.mFrom, status, code); // Update seq num
        tx = paymentTransaction(nAccounts, ledgerNum,
                                sourceAccountId); // re-generate the tx
        if (++numTries >= TX_SUBMIT_MAX_TRIES)
        {
            CLOG(ERROR, "LoadGen") << "Error submitting tx: did you specify "
                                      "correct number of accounts?";
            clear();
            return 0;
        }
    }

    nTxs -= 1;
    return nTxs;
}

void
LoadGenerator::inspectRate(uint32_t ledgerNum, uint32_t& txRate)
{
    // Automatic tx rate calculation involves taking the temperature
    // of the program and deciding if there's "room" to increase the
    // tx apply rate.
    auto& m = mApp.getMetrics();
    auto& ledgerCloseTimer = m.NewTimer({"ledger", "ledger", "close"});
    auto& ledgerAgeClosedTimer = m.NewTimer({"ledger", "age", "closed"});

    if (ledgerNum > 10 && ledgerCloseTimer.count() > 5)
    {
        // We consider the system "well loaded" at the point where its
        // ledger-close timer has avg duration within 10% of 2.5s
        // (or, well, "half the ledger-age target" which is 5s by
        // default).
        //
        // This is a bit arbitrary but it seems sufficient to
        // empirically differentiate "totally easy" from "starting to
        // struggle"; the system still has half the ledger-period to
        // digest incoming txs and acquire consensus. If it's over this
        // point, we reduce load; if it's under this point, we increase
        // load.
        //
        // We also decrease load (but don't increase it) based on ledger
        // age itself, directly: if the age gets above the herder's
        // timer target, we shed load accordingly because the *network*
        // (or some other component) is not reaching consensus fast
        // enough, independent of database close-speed.

        double targetAge =
            (double)Herder::EXP_LEDGER_TIMESPAN_SECONDS.count() * 1000.0;
        double actualAge = ledgerAgeClosedTimer.mean();

        if (mApp.getConfig().ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING)
        {
            targetAge = 1.0;
        }

        double targetLatency = targetAge / 2.0;
        double actualLatency = ledgerCloseTimer.mean();

        CLOG(INFO, "LoadGen")
            << "Considering auto-tx adjustment, avg close time "
            << ((uint32_t)actualLatency) << "ms, avg ledger age "
            << ((uint32_t)actualAge) << "ms";

        if (!maybeAdjustRate(targetAge, actualAge, txRate, false))
        {
            maybeAdjustRate(targetLatency, actualLatency, txRate, true);
        }

        if (txRate > 5000)
        {
            CLOG(WARNING, "LoadGen")
                << "TxRate > 5000, likely metric stutter, resetting";
            txRate = 10;
        }

        // Unfortunately the timer reservoir size is 1028 by default and
        // we cannot adjust it here, so in order to adapt to load
        // relatively quickly, we clear it out every 5 ledgers.
        ledgerAgeClosedTimer.Clear();
        ledgerCloseTimer.Clear();
    }
}

void
LoadGenerator::logProgress(std::chrono::nanoseconds submitTimer, bool isCreate,
                           uint32_t nAccounts, uint32_t nTxs,
                           uint32_t batchSize, uint32_t txRate)
{
    using namespace std::chrono;

    auto& m = mApp.getMetrics();
    auto& applyTx = m.NewTimer({"ledger", "transaction", "apply"});
    auto& applyOp = m.NewTimer({"transaction", "op", "apply"});

    auto submitSteps = duration_cast<milliseconds>(submitTimer).count();

    auto remainingTxCount = isCreate ? nAccounts / batchSize : nTxs;
    auto etaSecs =
        (uint32_t)(((double)remainingTxCount) / applyTx.one_minute_rate());

    auto etaHours = etaSecs / 3600;
    auto etaMins = etaSecs % 60;

    CLOG(INFO, "LoadGen") << "Tx/s: " << txRate << " target, "
                          << applyTx.one_minute_rate() << "tx/"
                          << applyOp.one_minute_rate() << "op actual (1m EWMA)."
                          << " Pending: " << nAccounts << " accounts, " << nTxs
                          << " txs."
                          << " ETA: " << etaHours << "h" << etaMins << "m";

    CLOG(DEBUG, "LoadGen") << "Step timing: " << submitSteps << "ms submit.";

    TxMetrics txm(mApp.getMetrics());
    txm.report();
}

LoadGenerator::TxInfo
LoadGenerator::creationTransaction(uint64_t startAccount, uint64_t numItems,
                                   uint32_t ledgerNum)
{
    vector<Operation> creationOps =
        createAccounts(startAccount, numItems, ledgerNum);
    TxInfo newTx = TxInfo{mRoot, creationOps};
    return newTx;
}

void
LoadGenerator::updateMinBalance()
{
    auto b = mApp.getLedgerManager().getMinBalance(0);
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
LoadGenerator::loadAccount(TestAccount& account, Database& database)
{
    AccountFrame::pointer ret;
    ret = AccountFrame::loadAccount(account.getPublicKey(), database);
    if (!ret)
    {
        return false;
    }
    account.setSequenceNumber(ret->getSeqNum());

    return true;
}

bool
LoadGenerator::loadAccount(TestAccountPtr acc, Database& database)
{
    if (acc)
    {
        return loadAccount(*acc, database);
    }
    return false;
}

std::pair<LoadGenerator::TestAccountPtr, LoadGenerator::TestAccountPtr>
LoadGenerator::pickAccountPair(uint32_t numAccounts, uint32_t ledgerNum,
                               uint64_t sourceAccountId)
{
    auto sourceAccount = findAccount(sourceAccountId, ledgerNum);

    // Mod with total number of accounts to ensure account exists
    uint64_t destAccountId =
        (sourceAccountId + sourceAccount->getLastSequenceNumber()) %
        numAccounts;
    auto destAccount = findAccount(destAccountId, ledgerNum);

    CLOG(DEBUG, "LoadGen") << "Generated pair for payment tx - "
                           << sourceAccountId << " and " << destAccountId;
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
        auto name = "TestAccount-" + to_string(accountId);
        auto account = TestAccount{mApp, txtest::getAccount(name.c_str()), sn};
        newAccountPtr = make_shared<TestAccount>(account);

        if (!loadAccount(newAccountPtr, mApp.getDatabase()))
        {
            std::runtime_error(
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

LoadGenerator::TxInfo
LoadGenerator::paymentTransaction(uint32_t numAccounts, uint32_t ledgerNum,
                                  uint64_t sourceAccount)
{
    TestAccountPtr to, from;
    uint64_t amount = 1;
    std::tie(from, to) = pickAccountPair(numAccounts, ledgerNum, sourceAccount);
    vector<Operation> paymentOps = {
        txtest::payment(to->getPublicKey(), amount)};
    TxInfo tx = TxInfo{from, paymentOps};

    return tx;
}

void
LoadGenerator::handleFailedSubmission(TestAccountPtr sourceAccount,
                                      Herder::TransactionSubmitStatus status,
                                      TransactionResultCode code)
{
    // Note that if transaction is a DUPLICATE, its sequence number is
    // incremented on the next call to execute.
    if (status == Herder::TX_STATUS_ERROR && code == txBAD_SEQ)
    {
        if (!loadAccount(sourceAccount, mApp.getDatabase()))
        {
            CLOG(ERROR, "LoadGen")
                << "Unable to reload account " << sourceAccount->getAccountId();
        }
    }
}

std::vector<LoadGenerator::TestAccountPtr>
LoadGenerator::checkAccountSynced(Database& database)
{
    std::vector<TestAccountPtr> result;
    for (auto const& acc : mAccounts)
    {
        TestAccountPtr account = acc.second;
        auto currentSeqNum = account->getLastSequenceNumber();
        auto reloadRes = loadAccount(account, database);
        // reload the account
        if (!reloadRes || currentSeqNum != account->getLastSequenceNumber())
        {
            CLOG(DEBUG, "LoadGen")
                << "Account " << account->getAccountId()
                << " is at sequence num " << currentSeqNum
                << ", but the DB is at  " << account->getLastSequenceNumber();
            result.push_back(account);
        }
    }
    return result;
}

void
LoadGenerator::waitTillComplete()
{
    if (!mLoadTimer)
    {
        mLoadTimer = make_unique<VirtualTimer>(mApp.getClock());
    }
    vector<TestAccountPtr> inconsistencies;
    inconsistencies = checkAccountSynced(mApp.getDatabase());

    if (inconsistencies.empty())
    {
        CLOG(INFO, "LoadGen") << "Load generation complete.";
        mApp.getMetrics()
            .NewMeter({"loadgen", "run", "complete"}, "run")
            .Mark();
        return;
    }
    else
    {
        mLoadTimer->expires_from_now(Herder::EXP_LEDGER_TIMESPAN_SECONDS);
        mLoadTimer->async_wait([this](asio::error_code const& error) {
            if (!error)
            {
                this->waitTillComplete();
            }
        });
    }
}

//////////////////////////////////////////////////////
// TxInfo
//////////////////////////////////////////////////////

LoadGenerator::TxMetrics::TxMetrics(medida::MetricsRegistry& m)
    : mAccountCreated(m.NewMeter({"loadgen", "account", "created"}, "account"))
    , mPayment(m.NewMeter({"loadgen", "payment", "any"}, "payment"))
    , mNativePayment(m.NewMeter({"loadgen", "payment", "native"}, "payment"))
    , mTxnAttempted(m.NewMeter({"loadgen", "txn", "attempted"}, "txn"))
    , mTxnRejected(m.NewMeter({"loadgen", "txn", "rejected"}, "txn"))
    , mTxnBytes(m.NewMeter({"loadgen", "txn", "bytes"}, "txn"))
{
}

void
LoadGenerator::TxMetrics::report()
{
    CLOG(DEBUG, "LoadGen") << "Counts: " << mTxnAttempted.count() << " tx, "
                           << mTxnRejected.count() << " rj, "
                           << mTxnBytes.count() << " by, "
                           << mAccountCreated.count() << " ac ("
                           << mPayment.count() << " pa ("
                           << mNativePayment.count() << " na, ";

    CLOG(DEBUG, "LoadGen") << "Rates/sec (1m EWMA): " << std::setprecision(3)
                           << mTxnAttempted.one_minute_rate() << " tx, "
                           << mTxnRejected.one_minute_rate() << " rj, "
                           << mTxnBytes.one_minute_rate() << " by, "
                           << mAccountCreated.one_minute_rate() << " ac, "
                           << mPayment.one_minute_rate() << " pa ("
                           << mNativePayment.one_minute_rate() << " na, ";
}

Herder::TransactionSubmitStatus
LoadGenerator::TxInfo::execute(Application& app, bool isCreate,
                               TransactionResultCode& code, int32_t batchSize)
{
    auto seqNum = mFrom->getLastSequenceNumber();
    mFrom->setSequenceNumber(seqNum + 1);

    TransactionFramePtr txf =
        transactionFromOperations(app, mFrom->getSecretKey(), seqNum + 1, mOps);
    TxMetrics txm(app.getMetrics());

    // Record tx metrics.
    if (isCreate)
    {
        while (batchSize--)
        {
            txm.mAccountCreated.Mark();
        }
    }
    else
    {
        txm.mPayment.Mark();
        txm.mNativePayment.Mark();
    }
    txm.mTxnAttempted.Mark();

    StellarMessage msg;
    msg.type(TRANSACTION);
    msg.transaction() = txf->getEnvelope();
    txm.mTxnBytes.Mark(xdr::xdr_argpack_size(msg));

    auto status = app.getHerder().recvTransaction(txf);
    if (status != Herder::TX_STATUS_PENDING)
    {
        CLOG(INFO, "LoadGen")
            << "tx rejected '" << Herder::TX_STATUS_STRING[status]
            << "': " << xdr::xdr_to_string(txf->getEnvelope()) << " ===> "
            << xdr::xdr_to_string(txf->getResult());
        if (status == Herder::TX_STATUS_ERROR)
        {
            code = txf->getResultCode();
        }
        txm.mTxnRejected.Mark();
    }
    else
    {
        app.getOverlayManager().broadcastMessage(msg);
    }

    return status;
}
}
