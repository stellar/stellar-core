// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "TxSetFrame.h"
#include "xdrpp/marshal.h"
#include "crypto/SHA.h"
#include "main/Application.h"
#include <algorithm>

namespace stellar
{

using namespace std;
    
TxSetFrame::TxSetFrame()
{
}

TxSetFrame::TxSetFrame(TransactionSet const& xdrSet)
{
    for (auto txEnvelope : xdrSet.txs)
    {
        TransactionFramePtr tx =
            TransactionFrame::makeTransactionFromWire(txEnvelope);
        mTransactions.push_back(tx);
    }
    mPreviousLedgerHash = xdrSet.previousLedgerHash;
}

struct HashTxSorter
{
    bool operator () (const TransactionFramePtr & tx1, const TransactionFramePtr & tx2)
    {
        // need to use the hash of whole tx here since multiple txs could have the same Contents
        return tx1->getFullHash() < tx2->getFullHash();
    }
};

// order the txset correctly
// must take into account multiple tx from same account
void TxSetFrame::sortForHash()
{
    std::sort(mTransactions.begin(), mTransactions.end(), HashTxSorter());
}

struct ApplyTxSorter
{
    Hash const& mSetHash;
    ApplyTxSorter(Hash const& h) : mSetHash(h) {}
    bool operator () (const TransactionFramePtr & tx1, const TransactionFramePtr & tx2)
    {
        // need to use the hash of whole tx here since multiple txs could have the same Contents
        Hash h1 = tx1->getFullHash();
        Hash h2 = tx2->getFullHash();
        Hash v1,v2;
        for(int n = 0; n < 32; n++)
        {
            v1[n] = mSetHash[n] ^ h1[n];
            v2[n] = mSetHash[n] ^ h2[n];
        }

        return v1 < v2;
    }
};

struct SeqSorter
{
    bool operator () (const TransactionFramePtr & tx1, const TransactionFramePtr & tx2)
    {
        return tx1->getSeqNum() < tx2->getSeqNum();
    }
};

void TxSetFrame::sortForApply(vector<TransactionFramePtr>& retList)
{
    vector< vector<TransactionFramePtr>> txLevels(4);
    map<uint256, vector<TransactionFramePtr>> accountTxMap;
    retList = mTransactions;
    // sort all the txs by seqnum
    std::sort(retList.begin(), retList.end(), SeqSorter());
    size_t maxLevel = 0;
    for(auto tx : retList)
    {
        auto &v = accountTxMap[tx->getSourceID()];

        if (v.size() > maxLevel)
        {
            maxLevel = v.size();
        }

        if (maxLevel >= txLevels.size())
        {
            txLevels.resize(maxLevel + 4);
        }
        txLevels[v.size()].push_back(tx);
        v.push_back(tx);
    }

    txLevels.resize(maxLevel + 1);

    retList.clear();
   
    for(auto level : txLevels)
    {
        ApplyTxSorter s(getContentsHash());
        std::sort(level.begin(), level.end(), s);
        for(auto tx : level)
        {
            retList.push_back(tx);
        }
    }
}

// need to make sure every account that is submitting a tx has enough to pay 
//  the fees of all the tx it has submitted in this set
// check seq num
bool TxSetFrame::checkValid(Application& app)
{
    // don't consider minBalance since you want to allow them to still send around credit etc
    map<uint256, vector<TransactionFramePtr>> accountTxMap;

    Hash lastHash;
    for(auto tx : mTransactions)
    {
        // make sure the set is sorted correctly
        if(tx->getFullHash() < lastHash) return false;
        accountTxMap[tx->getSourceID()].push_back(tx);
        lastHash = tx->getFullHash();
    }

    for(auto item : accountTxMap)
    {
        TransactionFramePtr first;
        for(auto tx : item.second)
        {
            if(!first)
            {    
                if(!tx->loadAccount(app)) return false;
                
                // make sure account can pay the fee for all these tx
                if(tx->getSourceAccount().getBalance() < 
                    (static_cast<int64_t>(xdr::size32(item.second.size())) * app.getLedgerGateway().getTxFee()))
					return false;
            } else
            {
                tx->getSourceAccount() = first->getSourceAccount();
            }
            
            if(tx->getSeqNum() < tx->getSourceAccount().getSeqNum() + 1) return false;
            first = tx;

            if(!tx->checkValid(app)) return false;
        }
    }
    return true;
}

Hash
TxSetFrame::getContentsHash()
{
    if (isZero(mHash))
    {
        sortForHash();
        SHA512_256 hasher;
        for(unsigned int n = 0; n < mTransactions.size(); n++)
        {
            hasher.add(xdr::xdr_to_msg(mTransactions[n]->getEnvelope()));
        }

        mHash = hasher.finish();
    }
    return mHash;
}



Hash
TxSetFrame::getPreviousLedgerHash()
{
  return mPreviousLedgerHash;
}

void
TxSetFrame::toXDR(TransactionSet& txSet)
{
    txSet.txs.resize(xdr::size32(mTransactions.size()));
    for (unsigned int n = 0; n < mTransactions.size(); n++)
    {
        txSet.txs[n]=mTransactions[n]->getEnvelope();
    }
}

}
