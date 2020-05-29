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

class LoadGenerator
{
  public:
    using TestAccountPtr = std::shared_ptr<TestAccount>;
    LoadGenerator(Application& app);

    // Generate one "step" worth of load (assuming 1 step per STEP_MSECS) at a
    // given target number of accounts and txs, and a given target tx/s rate.
    // If work remains after the current step, call scheduleLoadGeneration()
    // with the remainder.
    // txRate: The number of transactions per second when there is no spike.
    // spikeInterval: A spike will occur every spikeInterval seconds.
    //                Set this to 0 if no spikes are needed.
    // spikeSize: The number of transactions a spike injects on top of the
    // steady rate.
    void generateLoad(bool isCreate, uint32_t nAccounts, uint32_t offset,
                      uint32_t nTxs, uint32_t txRate, uint32_t batchSize,
                      std::chrono::seconds spikeInterval, uint32_t spikeSize);

    // Verify cached accounts are properly reflected in the database
    // return any accounts that are inconsistent.
    std::vector<TestAccountPtr> checkAccountSynced(Application& app,
                                                   bool isCreate);

  private:
    struct TxMetrics
    {
        medida::Meter& mAccountCreated;
        medida::Meter& mNativePayment;
        medida::Meter& mTxnAttempted;
        medida::Meter& mTxnRejected;
        medida::Meter& mTxnBytes;

        TxMetrics(medida::MetricsRegistry& m);
        void report();
    };

    struct TxInfo
    {
        TestAccountPtr mFrom;
        std::vector<Operation> mOps;
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
        TransactionQueue::AddResult execute(Application& app, bool isCreate,
                                            TransactionResultCode& code,
                                            int32_t batchSize);
    };

    static const uint32_t STEP_MSECS;
    static const uint32_t TX_SUBMIT_MAX_TRIES;
    static const uint32_t TIMEOUT_NUM_LEDGERS;

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

    medida::Meter& mLoadgenComplete;
    medida::Meter& mLoadgenFail;

    bool mFailed{false};
    int mWaitTillCompleteForLedgers{0};

    void reset();
    void createRootAccount();
    int64_t getTxPerStep(uint32_t txRate, std::chrono::seconds spikeInterval,
                         uint32_t spikeSize);

    // Schedule a callback to generateLoad() STEP_MSECS miliseconds from now.
    void scheduleLoadGeneration(bool isCreate, uint32_t nAccounts,
                                uint32_t offset, uint32_t nTxs, uint32_t txRate,
                                uint32_t batchSize,
                                std::chrono::seconds spikeInterval,
                                uint32_t spikeSize);

    std::vector<Operation> createAccounts(uint64_t i, uint64_t batchSize,
                                          uint32_t ledgerNum);
    bool loadAccount(TestAccount& account, Application& app);
    bool loadAccount(TestAccountPtr account, Application& app);

    std::pair<TestAccountPtr, TestAccountPtr>
    pickAccountPair(uint32_t numAccounts, uint32_t offset, uint32_t ledgerNum,
                    uint64_t sourceAccountId);
    TestAccountPtr findAccount(uint64_t accountId, uint32_t ledgerNum);
    LoadGenerator::TxInfo paymentTransaction(uint32_t numAccounts,
                                             uint32_t offset,
                                             uint32_t ledgerNum,
                                             uint64_t sourceAccount);
    void maybeHandleFailedTx(TestAccountPtr sourceAccount,
                             TransactionQueue::AddResult status,
                             TransactionResultCode code);
    TxInfo creationTransaction(uint64_t startAccount, uint64_t numItems,
                               uint32_t ledgerNum);
    void logProgress(std::chrono::nanoseconds submitTimer, bool isCreate,
                     uint32_t nAccounts, uint32_t nTxs, uint32_t batchSize,
                     uint32_t txRate);

    uint32_t submitCreationTx(uint32_t nAccounts, uint32_t offset,
                              uint32_t batchSize, uint32_t ledgerNum);
    uint32_t submitPaymentTx(uint32_t nAccounts, uint32_t offset,
                             uint32_t batchSize, uint32_t ledgerNum,
                             uint32_t nTxs);
    void waitTillComplete(bool isCreate);

    void updateMinBalance();
};
}
