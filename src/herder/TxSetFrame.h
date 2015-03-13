#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

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

    TxSetFrame();
    // make it from the wire
    TxSetFrame(TransactionSet const& xdrSet);

    // returns the hash of this tx set
    Hash getContentsHash();

    Hash &previousLedgerHash();
    Hash const& previousLedgerHash() const;

    void sortForHash();

    std::vector<TransactionFramePtr> sortForApply();

    bool checkValid(Application& app);

    void add(TransactionFramePtr tx)
    {
        mTransactions.push_back(tx);
        mHashIsValid = false;
    }

    size_t size()
    {
        return mTransactions.size();
    }

    void toXDR(TransactionSet& set);
};
}
