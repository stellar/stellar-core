#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerHashUtils.h"
#include "overlay/StellarXDR.h"
#include "transactions/TransactionFrame.h"
#include <deque>
#include <functional>

namespace stellar
{
class Application;

class TxSetFrame;
using TxSetFrameConstPtr = std::shared_ptr<TxSetFrame const>;

// A wrapper for a set of transactions that maintains the hash order
class TxSetFrame
{
  public:
    using AccountTransactionQueue = std::deque<TransactionFrameBasePtr>;
    using Transactions = std::vector<TransactionFrameBasePtr>;

    TxSetFrame(Hash const& previousLedgerHash,
               Transactions const& transactions);

    TxSetFrame(Hash const& previousLedgerHash);

    // make it from the wire
    TxSetFrame(Hash const& networkID, TransactionSet const& xdrSet);

    TxSetFrame(TxSetFrame const& other) = default;

    virtual ~TxSetFrame(){};

    // returns the hash of this tx set
    Hash const&
    getContentsHash() const
    {
        return mHash;
    }

    Hash const&
    previousLedgerHash() const
    {
        return mPreviousLedgerHash;
    }

    Transactions const&
    getTxsInHashOrder() const
    {
        return mTxsInHashOrder;
    }

    virtual Transactions getTxsInApplyOrder() const;

    bool checkValid(Application& app, uint64_t lowerBoundCloseTimeOffset,
                    uint64_t upperBoundCloseTimeOffset) const;

    size_t size(LedgerHeader const& lh) const;

    size_t
    sizeTx() const
    {
        return mTxsInHashOrder.size();
    }

    size_t sizeOp() const;

    // return the base fee associated with this transaction set
    int64_t getBaseFee(LedgerHeader const& lh) const;

    // return the sum of all fees that this transaction set would take
    int64_t getTotalFees(LedgerHeader const& lh) const;
    void toXDR(TransactionSet& set) const;

  protected:
    Hash const mPreviousLedgerHash;

    Transactions const mTxsInHashOrder;

    Hash const mHash;

    friend struct SurgeCompare;
};

} // namespace stellar
