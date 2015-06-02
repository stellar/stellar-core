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

class LoadGenerator
{
public:
    LoadGenerator();
    ~LoadGenerator();

    struct TxInfo;
    struct AccountInfo;
    using AccountInfoPtr = std::shared_ptr<AccountInfo>;

    std::vector<AccountInfoPtr> mAccounts;
    uint64 mMinBalance;

    std::vector<TxInfo> accountCreationTransactions(size_t n);
    AccountInfoPtr createAccount(size_t i);
    std::vector<AccountInfoPtr> createAccounts(size_t n);
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

        void execute(Application& app);
        TransactionFramePtr createPaymentTx();
        void recordExecution(uint64_t baseFee);
    };
};

}
