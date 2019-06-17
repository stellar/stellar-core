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
#include <unordered_set>
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
    enum class AddResult
    {
        ADD_STATUS_PENDING = 0,
        ADD_STATUS_DUPLICATE,
        ADD_STATUS_ERROR,
        ADD_STATUS_TRY_AGAIN_LATER,
        ADD_STATUS_COUNT
    };

    struct AccountTxQueueInfo
    {
        SequenceNumber mMaxSeq{0};
        int64_t mTotalFees{0};

        friend bool operator==(AccountTxQueueInfo const& x,
                               AccountTxQueueInfo const& y);
    };

    struct TxMap
    {
        AccountTxQueueInfo mCurrentQInfo;
        std::unordered_map<Hash, TransactionFramePtr> mTransactions;
        void addTx(TransactionFramePtr);
        void recalculate();
    };
    typedef std::unordered_map<AccountID, std::shared_ptr<TxMap>> AccountTxMap;

    explicit TransactionQueue(Application& app, int pendingDepth, int banDepth);

    AddResult tryAdd(TransactionFramePtr tx);
    // it is the responsibility of the caller to always remove such sets of
    // transactions that remaining ones have their sequence numbers increasing
    // by one
    void remove(std::vector<TransactionFramePtr> const& txs);
    // remove oldest transactions and move all other transactions to slots older
    // by one, this results in newest queue slot being empty
    void shift();

    AccountTxQueueInfo
    getAccountTransactionQueueInfo(AccountID const& accountID) const;

    int countBanned(int index) const;
    bool isBanned(Hash const& hash) const;
    std::shared_ptr<TxSetFrame> toTxSet(Hash const& lclHash) const;

  private:
    Application& mApp;
    std::vector<medida::Counter*> mSizeByAge;
    std::deque<AccountTxMap> mPendingTransactions;
    std::deque<std::unordered_set<Hash>> mBannedTransactions;

    bool contains(TransactionFramePtr tx) const;
};

static const char* TX_STATUS_STRING[static_cast<int>(
    TransactionQueue::AddResult::ADD_STATUS_COUNT)] = {
    "PENDING", "DUPLICATE", "ERROR", "TRY_AGAIN_LATER"};
}
