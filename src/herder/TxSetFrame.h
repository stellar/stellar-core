#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "generated/StellarXDR.h"
#include "transactions/TransactionFrame.h"

namespace stellar
{
class Application;

class TransactionFrame;
typedef std::shared_ptr<TransactionFrame> TransactionFramePtr;

class TxSetFrame
{
    bool mHashIsValid;
    Hash mHash;

    Hash mPreviousLedgerHash;

  public:
    typedef std::shared_ptr<TxSetFrame> pointer;

    std::vector<TransactionFramePtr> mTransactions;

    TxSetFrame(Hash const& previousLedgerHash);
    // make it from the wire
    TxSetFrame(TransactionSet const& xdrSet);

    // returns the hash of this tx set
    Hash getContentsHash();

    Hash& previousLedgerHash();
    Hash const& previousLedgerHash() const;

    void sortForHash();

    std::vector<TransactionFramePtr> sortForApply();

    bool checkValid(Application& app) const;
    void trimInvalid(Application& app, std::vector<TransactionFramePtr> trimmed);

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
}
