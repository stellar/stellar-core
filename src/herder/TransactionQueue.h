#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SecretKey.h"
#include "herder/TxSetFrame.h"
#include "transactions/TransactionFrame.h"
#include "util/HashOfHash.h"
#include "util/XDROperators.h"
#include "xdr/Stellar-transaction.h"

#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

namespace medida
{
class Counter;
}

namespace stellar
{

class Application;

struct AccountTransactionsQueueState
{
    SequenceNumber mMaxSeq{0};
    int64_t mTotalFees{0};

    friend bool operator==(AccountTransactionsQueueState const& x,
                           AccountTransactionsQueueState const& y);
};

class TransactionQueue
{
  public:
    enum class AddResult
    {
        STATUS_PENDING = 0,
        STATUS_DUPLICATE,
        STATUS_ERROR,
        STATUS_COUNT
    };

    struct TxMap
    {
        AccountTransactionsQueueState mCurrentState;
        std::unordered_map<Hash, TransactionFramePtr> mTransactions;
        void addTx(TransactionFramePtr);
        void recalculate();
    };
    typedef std::unordered_map<AccountID, std::shared_ptr<TxMap>> AccountTxMap;

    explicit TransactionQueue(Application& app, int pendingDepth);

    AddResult tryAdd(TransactionFramePtr tx);
    // it is responsibility of the caller to always remove such sets of
    // transactions that remaining ones have theis sequence numbers increasing
    // by one
    void remove(std::vector<TransactionFramePtr> const& txs);
    // remove oldest transactions and move all other transactions to slots older
    // by one, this results in newest queue slot being empty
    void shift();

    AccountTransactionsQueueState
    getAccountTransactionQueueState(AccountID const& accountID) const;

    std::shared_ptr<TxSetFrame> toTxSet(Hash const& lclHash) const;

  private:
    Application& mApp;
    std::vector<medida::Counter*> mSizeByAge;
    std::deque<AccountTxMap> mPendingTransactions;

    bool contains(TransactionFramePtr tx) const;
};

static const char* TX_STATUS_STRING[static_cast<int>(
    TransactionQueue::AddResult::STATUS_COUNT)] = {"PENDING", "DUPLICATE",
                                                   "ERROR"};
}
