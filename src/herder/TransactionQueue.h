#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SecretKey.h"
#include "herder/TxSetFrame.h"
#include "util/HashOfHash.h"
#include <memory>
#include <vector>

namespace medida
{
class Counter;
}

namespace stellar
{
class Application;

class TransactionQueue
{
  public:
    explicit TransactionQueue(Application& app, size_t pendingDepth,
                              size_t banDepth);

#ifdef BUILD_TESTS
    size_t countBanned(int index) const;
#endif

    bool isBanned(Hash const& hash) const;

    void removeApplied(std::vector<TransactionFrameBasePtr> const& dropTxs);

    void removeTrimmed(std::vector<TransactionFrameBasePtr> const& dropTxs);

    void shift();

    std::shared_ptr<TxSetFrame> toTxSet(Hash const& lclHash) const;

    enum class AddResult
    {
        ADD_STATUS_PENDING = 0,
        ADD_STATUS_DUPLICATE,
        ADD_STATUS_ERROR,
        ADD_STATUS_TRY_AGAIN_LATER,
        ADD_STATUS_COUNT
    };
    AddResult tryAdd(TransactionFrameBasePtr const& tx);

#ifdef BUILD_TESTS
    struct AccountTxQueueInfo
    {
        SequenceNumber mMaxSeq{0};
        int64_t mTotalFees{0};
        int mAge{0};
    };
    AccountTxQueueInfo
    getAccountTransactionQueueInfo(AccountID const& acc) const;
#endif

  private:
    struct AccountTransactions
    {
        using Transactions = std::vector<std::vector<TransactionFrameBasePtr>>;

        int mAge{0};
        Transactions mTransactions;
    };

    Application& mApp;
    size_t mPendingDepth;
    std::vector<medida::Counter*> mSizeByAge;

    using PendingTransactions =
        std::unordered_map<AccountID, AccountTransactions>;
    PendingTransactions mPendingTransactions;

    std::unordered_map<AccountID, int64_t> mTotalFees;

    using BannedTransactions = std::deque<std::unordered_set<Hash>>;
    BannedTransactions mBannedTransactions;

    void addFee(TransactionFrameBasePtr const& tx);

    AccountTransactions::Transactions::iterator
    findTx(AccountTransactions::Transactions& transactions,
           TransactionFrameBasePtr const& tx);

    int64_t getTotalFee(AccountID const& feeSource) const;

    void removeFee(TransactionFrameBasePtr const& tx);

    AddResult validateAndMaybeAddTransaction(TransactionFrameBasePtr const& tx,
                                             int64_t validationSeqNum);
};

static const char* TX_STATUS_STRING[static_cast<int>(
    TransactionQueue::AddResult::ADD_STATUS_COUNT)] = {
    "PENDING", "DUPLICATE", "ERROR", "TRY_AGAIN_LATER"};
}
