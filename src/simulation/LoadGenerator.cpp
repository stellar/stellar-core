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
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/Timer.h"
#include "util/numeric.h"
#include "util/types.h"

#include "database/Database.h"

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
    : mMinBalance(0), mLastSecond(0), mApp(app), mTotalSubmitted(0)
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
        if (!loadAccount(mRoot, mApp))
        {
            CLOG(ERROR, "LoadGen") << "Could not retrieve root account!";
        }
    }
}

int64_t
LoadGenerator::getTxPerStep(uint32_t txRate)
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

    if (txs <= mTotalSubmitted)
    {
        return 0;
    }

    return txs - mTotalSubmitted;
}

void
LoadGenerator::clear()
{
    mAccounts.clear();
    mRoot.reset();
    mStartTime.reset();
    mTotalSubmitted = 0;
}

// Schedule a callback to generateLoad() STEP_MSECS miliseconds from now.
void
LoadGenerator::scheduleLoadGeneration(bool isCreate, uint32_t nAccounts,
                                      uint32_t offset, uint32_t nTxs,
                                      uint32_t txRate, uint32_t batchSize)
{
    if (!mLoadTimer)
    {
        mLoadTimer = std::make_unique<VirtualTimer>(mApp.getClock());
    }

    if (mApp.getState() == Application::APP_SYNCED_STATE)
    {
        mLoadTimer->expires_from_now(std::chrono::milliseconds(STEP_MSECS));
        mLoadTimer->async_wait(
            [this, nAccounts, offset, nTxs, txRate, batchSize, isCreate]() {
                this->generateLoad(isCreate, nAccounts, offset, nTxs, txRate,
                                   batchSize);
            },
            &VirtualTimer::onFailureNoop);
    }
    else
    {
        CLOG(WARNING, "LoadGen")
            << "Application is not in sync, load generation inhibited. State "
            << mApp.getState();
        mLoadTimer->expires_from_now(std::chrono::seconds(10));
        mLoadTimer->async_wait(
            [this, nAccounts, offset, nTxs, txRate, batchSize, isCreate]() {
                this->scheduleLoadGeneration(isCreate, nAccounts, offset, nTxs,
                                             txRate, batchSize);
            },
            &VirtualTimer::onFailureNoop);
    }
}

// Generate one "step" worth of load (assuming 1 step per STEP_MSECS) at a
// given target number of accounts and txs, and a given target tx/s rate.
// If work remains after the current step, call scheduleLoadGeneration()
// with the remainder.
void
LoadGenerator::generateLoad(bool isCreate, uint32_t nAccounts, uint32_t offset,
                            uint32_t nTxs, uint32_t txRate, uint32_t batchSize)
{
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

    auto txPerStep = getTxPerStep(txRate);
    auto& submitTimer =
        mApp.getMetrics().NewTimer({"loadgen", "step", "submit"});
    auto submitScope = submitTimer.TimeScope();

    uint32_t ledgerNum = mApp.getLedgerManager().getLastClosedLedgerNum() + 1;

    for (int64_t i = 0; i < txPerStep; ++i)
    {
        if (isCreate)
        {
            nAccounts =
                submitCreationTx(nAccounts, offset, batchSize, ledgerNum);
        }
        else
        {
            nTxs =
                submitPaymentTx(nAccounts, offset, batchSize, ledgerNum, nTxs);
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

    // Emit a log message once per second.
    if (now != mLastSecond)
    {
        logProgress(submit, isCreate, nAccounts, nTxs, batchSize, txRate);
    }

    mLastSecond = now;
    mTotalSubmitted += txPerStep;
    scheduleLoadGeneration(isCreate, nAccounts, offset, nTxs, txRate,
                           batchSize);
}

uint32_t
LoadGenerator::submitCreationTx(uint32_t nAccounts, uint32_t offset,
                                uint32_t batchSize, uint32_t ledgerNum)
{
    uint32_t numToProcess = nAccounts < batchSize ? nAccounts : batchSize;
    TxInfo tx =
        creationTransaction(mAccounts.size() + offset, numToProcess, ledgerNum);
    TransactionResultCode code;
    TransactionQueue::AddResult status;
    bool createDuplicate = false;
    int numTries = 0;

    while ((status = tx.execute(mApp, true, code, batchSize)) !=
           TransactionQueue::AddResult::STATUS_PENDING)
    {
        handleFailedSubmission(tx.mFrom, status, code); // Update seq num
        if (status == TransactionQueue::AddResult::STATUS_DUPLICATE)
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
LoadGenerator::submitPaymentTx(uint32_t nAccounts, uint32_t offset,
                               uint32_t batchSize, uint32_t ledgerNum,
                               uint32_t nTxs)
{
    auto sourceAccountId = rand_uniform<uint64_t>(0, nAccounts - 1) + offset;
    TxInfo tx =
        paymentTransaction(nAccounts, offset, ledgerNum, sourceAccountId);

    TransactionResultCode code;
    TransactionQueue::AddResult status;
    int numTries = 0;

    while ((status = tx.execute(mApp, false, code, batchSize)) !=
           TransactionQueue::AddResult::STATUS_PENDING)
    {
        handleFailedSubmission(tx.mFrom, status, code); // Update seq num
        tx = paymentTransaction(nAccounts, offset, ledgerNum,
                                sourceAccountId); // re-generate the tx
        if (++numTries >= TX_SUBMIT_MAX_TRIES)
        {
            CLOG(ERROR, "LoadGen") << "Error submitting tx: did you specify "
                                      "correct number of accounts and offset?";
            clear();
            return 0;
        }
    }

    nTxs -= 1;
    return nTxs;
}

void
LoadGenerator::logProgress(std::chrono::nanoseconds submitTimer, bool isCreate,
                           uint32_t nAccounts, uint32_t nTxs,
                           uint32_t batchSize, uint32_t txRate)
{
    using namespace std::chrono;

    auto& m = mApp.getMetrics();
    auto& applyTx = m.NewTimer({"ledger", "transaction", "apply"});
    auto& applyOp = m.NewTimer({"ledger", "operation", "apply"});

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

        if (!loadAccount(newAccountPtr, mApp))
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
LoadGenerator::paymentTransaction(uint32_t numAccounts, uint32_t offset,
                                  uint32_t ledgerNum, uint64_t sourceAccount)
{
    TestAccountPtr to, from;
    uint64_t amount = 1;
    std::tie(from, to) =
        pickAccountPair(numAccounts, offset, ledgerNum, sourceAccount);
    vector<Operation> paymentOps = {
        txtest::payment(to->getPublicKey(), amount)};
    TxInfo tx = TxInfo{from, paymentOps};

    return tx;
}

void
LoadGenerator::handleFailedSubmission(TestAccountPtr sourceAccount,
                                      TransactionQueue::AddResult status,
                                      TransactionResultCode code)
{
    // Note that if transaction is a DUPLICATE, its sequence number is
    // incremented on the next call to execute.
    if (status == TransactionQueue::AddResult::STATUS_ERROR &&
        code == txBAD_SEQ)
    {
        if (!loadAccount(sourceAccount, mApp))
        {
            CLOG(ERROR, "LoadGen")
                << "Unable to reload account " << sourceAccount->getAccountId();
        }
    }
}

std::vector<LoadGenerator::TestAccountPtr>
LoadGenerator::checkAccountSynced(Application& app)
{
    std::vector<TestAccountPtr> result;
    for (auto const& acc : mAccounts)
    {
        TestAccountPtr account = acc.second;
        auto currentSeqNum = account->getLastSequenceNumber();
        auto reloadRes = loadAccount(account, app);
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
        mLoadTimer = std::make_unique<VirtualTimer>(mApp.getClock());
    }
    vector<TestAccountPtr> inconsistencies;
    inconsistencies = checkAccountSynced(mApp);

    if (inconsistencies.empty())
    {
        CLOG(INFO, "LoadGen") << "Load generation complete.";
        mApp.getMetrics()
            .NewMeter({"loadgen", "run", "complete"}, "run")
            .Mark();
        clear();
        return;
    }
    else
    {
        mLoadTimer->expires_from_now(
            mApp.getConfig().getExpectedLedgerCloseTime());
        mLoadTimer->async_wait([this]() { this->waitTillComplete(); },
                               &VirtualTimer::onFailureNoop);
    }
}

//////////////////////////////////////////////////////
// TxInfo
//////////////////////////////////////////////////////

LoadGenerator::TxMetrics::TxMetrics(medida::MetricsRegistry& m)
    : mAccountCreated(m.NewMeter({"loadgen", "account", "created"}, "account"))
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
                           << mNativePayment.count() << " na, ";

    CLOG(DEBUG, "LoadGen") << "Rates/sec (1m EWMA): " << std::setprecision(3)
                           << mTxnAttempted.one_minute_rate() << " tx, "
                           << mTxnRejected.one_minute_rate() << " rj, "
                           << mTxnBytes.one_minute_rate() << " by, "
                           << mAccountCreated.one_minute_rate() << " ac, "
                           << mNativePayment.one_minute_rate() << " na, ";
}

TransactionQueue::AddResult
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
        txm.mNativePayment.Mark();
    }
    txm.mTxnAttempted.Mark();

    StellarMessage msg;
    msg.type(TRANSACTION);
    msg.transaction() = txf->getEnvelope();
    txm.mTxnBytes.Mark(xdr::xdr_argpack_size(msg));

    auto status = app.getHerder().recvTransaction(txf);
    if (status != TransactionQueue::AddResult::STATUS_PENDING)
    {
        CLOG(INFO, "LoadGen")
            << "tx rejected '" << TX_STATUS_STRING[static_cast<int>(status)]
            << "': " << xdr::xdr_to_string(txf->getEnvelope()) << " ===> "
            << xdr::xdr_to_string(txf->getResult());
        if (status == TransactionQueue::AddResult::STATUS_ERROR)
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
