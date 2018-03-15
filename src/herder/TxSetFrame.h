#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/StellarXDR.h"
#include "transactions/TransactionFrame.h"
#include <functional>

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

    bool
    checkOrTrim(Application& app,
                std::function<bool(TransactionFramePtr, SequenceNumber)>
                    processInvalidTxLambda,
                std::function<bool(std::vector<TransactionFramePtr> const&)>
                    processLastInvalidTxLambda);

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
    void trimInvalid(Application& app,
                     std::vector<TransactionFramePtr>& trimmed);
    void surgePricingFilter(Application& app);

    void removeTx(TransactionFramePtr tx);

    void
    add(TransactionFramePtr tx)
    {
        mTransactions.push_back(tx);
        mHashIsValid = false;
    }

    size_t
    size()
    {
        return mTransactions.size();
    }

    void toXDR(TransactionSet& set);
};
} // namespace stellar
