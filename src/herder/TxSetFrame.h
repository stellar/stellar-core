#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerHashUtils.h"
#include "overlay/StellarXDR.h"
#include "transactions/TransactionFrame.h"
#include <deque>
#include <functional>
#include <unordered_map>

namespace stellar
{
class Application;

class TxSetFrame;
typedef std::shared_ptr<TxSetFrame> TxSetFramePtr;

class TxSetFrame
{
    bool mHashIsValid;
    Hash mHash;

    Hash mPreviousLedgerHash;

    using AccountTransactionQueue = std::deque<TransactionFramePtr>;

    bool checkOrTrim(Application& app,
                     std::function<bool(TransactionFramePtr, SequenceNumber)>
                         processInvalidTxLambda,
                     std::function<bool(std::deque<TransactionFramePtr> const&)>
                         processLastInvalidTxLambda);

    std::unordered_map<AccountID, AccountTransactionQueue>
    buildAccountTxQueues();
    friend struct SurgeCompare;

  public:
    std::vector<TransactionFramePtr> mTransactions;

    TxSetFrame(Hash const& previousLedgerHash);

    TxSetFrame(TxSetFrame const& other) = default;

    // make it from the wire
    TxSetFrame(Hash const& networkID, TransactionSet const& xdrSet);

    // returns the hash of this tx set
    Hash getContentsHash();

    Hash& previousLedgerHash();
    Hash const& previousLedgerHash() const;

    void sortForHash();

    std::vector<TransactionFramePtr> sortForApply();

    bool checkValid(Application& app);

    // remove invalid transaction from this set and return those removed
    // transactions
    std::vector<TransactionFramePtr> trimInvalid(Application& app);
    void surgePricingFilter(Application& app);

    void removeTx(TransactionFramePtr tx);

    void
    add(TransactionFramePtr tx)
    {
        mTransactions.push_back(tx);
        mHashIsValid = false;
    }

    size_t size(LedgerHeader const& lh) const;

    size_t
    sizeTx() const
    {
        return mTransactions.size();
    }

    size_t sizeOp() const;

    // return the base fee associated with this transaction set
    int64_t getBaseFee(LedgerHeader const& lh) const;

    // return the sum of all fees that this transaction set would take
    int64_t getTotalFees(LedgerHeader const& lh) const;
    void toXDR(TransactionSet& set);
};
} // namespace stellar
