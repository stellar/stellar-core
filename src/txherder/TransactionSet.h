#ifndef __TRANSACTIONSET__
#define __TRANSACTIONSET__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "transactions/Transaction.h"
#include "generated/StellarXDR.h"

using namespace std;

namespace stellar
{

class Transaction;
typedef std::shared_ptr<Transaction> TransactionPtr;

class TransactionSet
{
    stellarxdr::uint256 mHash;

  public:
    typedef std::shared_ptr<TransactionSet> pointer;

    vector<TransactionPtr> mTransactions;

    TransactionSet();

    // make it from the wire
    TransactionSet(stellarxdr::TransactionSet const& xdrSet);

    TransactionPtr getTransaction(stellarxdr::uint256 const& txHash);
    // returns the hash of this tx set
    stellarxdr::uint256 getContentsHash();

    void
    add(TransactionPtr tx)
    {
        mTransactions.push_back(tx);
    }

    void store();

    uint32_t
    size()
    {
        return mTransactions.size();
    }

    void toXDR(stellarxdr::TransactionSet& set);
};
}

#endif // !__TRANSACTIONSET__
