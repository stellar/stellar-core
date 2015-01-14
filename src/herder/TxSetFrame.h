#ifndef __TRANSACTIONSET__
#define __TRANSACTIONSET__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "transactions/TransactionFrame.h"
#include "generated/StellarXDR.h"

using namespace std;

namespace stellar
{
class Application;

class TransactionFrame;
typedef std::shared_ptr<TransactionFrame> TransactionFramePtr;

class TxSetFrame
{
    uint256 mHash;

  public:
    typedef std::shared_ptr<TxSetFrame> pointer;

    vector<TransactionFramePtr> mTransactions;
    uint256                     mPreviousLedgerHash;

    TxSetFrame();
    // make it from the wire
    TxSetFrame(TransactionSet const& xdrSet);

    // returns the hash of this tx set
    Hash getContentsHash();
    Hash getPreviousLedgerHash();

    void sortForHash();

    void sortForApply(vector<TransactionFramePtr>& retList);

    bool checkValid(Application& app);

    void
    add(TransactionFramePtr tx)
    {
        mTransactions.push_back(tx);
    }

    size_t
    size()
    {
        return mTransactions.size();
    }

    void toXDR(TransactionSet& set);
};
}

#endif // !__TRANSACTIONSET__
