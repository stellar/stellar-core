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
#include <util/format.h>
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
    LoadGenerator(Application& app);
    ~LoadGenerator();
    void clear();
    bool maybeAdjustRate(double target, double actual, uint32_t& rate,
                         bool increaseOk);
    void inspectRate(uint32_t ledgerNum, uint32_t& txRate);

    struct TxInfo;
    using TestAccountPtr = std::shared_ptr<TestAccount>;

    static const uint32_t STEP_MSECS;
    static const uint32_t TX_SUBMIT_MAX_TRIES;

    std::unique_ptr<VirtualTimer> mLoadTimer;
    int64 mMinBalance;
    uint64_t mLastSecond;

    void createRootAccount();
    uint32_t getTxPerStep(uint32_t txRate);

    // Schedule a callback to generateLoad() STEP_MSECS miliseconds from now.
    void scheduleLoadGeneration(bool isCreate, uint32_t nAccounts,
                                uint32_t offset, uint32_t nTxs, uint32_t txRate,
                                uint32_t batchSize, bool autoRate);

    // Generate one "step" worth of load (assuming 1 step per STEP_MSECS) at a
    // given target number of accounts and txs, and a given target tx/s rate.
    // If work remains after the current step, call scheduleLoadGeneration()
    // with the remainder.
    void generateLoad(bool isCreate, uint32_t nAccounts, uint32_t offset,
                      uint32_t nTxs, uint32_t txRate, uint32_t batchSize,
                      bool autoRate);

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
    void handleFailedSubmission(TestAccountPtr sourceAccount,
                                Herder::TransactionSubmitStatus status,
                                TransactionResultCode code);
    TxInfo creationTransaction(uint64_t startAccount, uint64_t numItems,
                               uint32_t ledgerNum);
    std::vector<TestAccountPtr> checkAccountSynced(Application& app);
    void logProgress(std::chrono::nanoseconds submitTimer, bool isCreate,
                     uint32_t nAccounts, uint32_t nTxs, uint32_t batchSize,
                     uint32_t txRate);

    uint32_t submitCreationTx(uint32_t nAccounts, uint32_t offset,
                              uint32_t batchSize, uint32_t ledgerNum);
    uint32_t submitPaymentTx(uint32_t nAccounts, uint32_t offset,
                             uint32_t batchSize, uint32_t ledgerNum,
                             uint32_t nTxs);

    void updateMinBalance();
    void waitTillComplete();

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
        Herder::TransactionSubmitStatus execute(Application& app, bool isCreate,
                                                TransactionResultCode& code,
                                                int32_t batchSize);
    };

  protected:
    Application& mApp;
    TestAccountPtr mRoot;
    // Accounts cache
    std::map<uint64_t, TestAccountPtr> mAccounts;
};
}
