#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Application.h"
#include "crypto/SecretKey.h"
#include "transactions/TxTests.h"
#include "generated/Stellar-types.h"
#include <vector>

namespace stellar
{

class VirtualTimer;

class LoadGenerator
{
public:
    LoadGenerator();
    ~LoadGenerator();

    struct TxInfo;
    struct AccountInfo;
    using AccountInfoPtr = std::shared_ptr<AccountInfo>;

    static const uint32_t STEP_MSECS;

    std::vector<AccountInfoPtr> mAccounts;
    std::unique_ptr<VirtualTimer> mLoadTimer;
    uint64 mMinBalance;

    // Schedule a callback to generateLoad() STEP_MSECS miliseconds from now.
    void scheduleLoadGeneration(Application& app,
                                uint32_t nAccounts,
                                uint32_t nTxs,
                                uint32_t txRate);

    // Generate one "step" worth of load (assuming 1 step per STEP_MSECS) at a
    // given target number of accounts and txs, and a given target tx/s rate.
    // If work remains after the current step, call scheduleLoadGeneration()
    // with the remainder.
    void generateLoad(Application& app,
                      uint32_t nAccounts,
                      uint32_t nTxs,
                      uint32_t txRate);

    std::vector<TxInfo> accountCreationTransactions(size_t n);
    AccountInfoPtr createAccount(size_t i);
    std::vector<AccountInfoPtr> createAccounts(size_t n);
    bool loadAccount(Application& app, AccountInfo& account);

    TxInfo createTransferTransaction(size_t iFrom, size_t iTo, uint64_t amount);
    TxInfo createRandomTransaction(float alpha);
    std::vector<TxInfo> createRandomTransactions(size_t n, float paretoAlpha);
    void updateMinBalance(Application& app);

    struct AccountInfo : public std::enable_shared_from_this<AccountInfo>
    {
        AccountInfo(size_t id, SecretKey key, uint64_t balance,
                    SequenceNumber seq, LoadGenerator& loadGen);
        size_t mId;
        SecretKey mKey;
        uint64_t mBalance;
        SequenceNumber mSeq;

        TxInfo creationTransaction();

      private:
        LoadGenerator& mLoadGen;
    };

    struct TxInfo
    {
        AccountInfoPtr mFrom;
        AccountInfoPtr mTo;
        bool mCreate;
        uint64_t mAmount;

        bool execute(Application& app);
        TransactionFramePtr createPaymentTx();
        void recordExecution(uint64_t baseFee);
    };
};

}
