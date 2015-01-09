// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "TxSetFrame.h"
#include "xdrpp/marshal.h"
#include "crypto/SHA.h"
#include "main/Application.h"

namespace stellar
{
    
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
}

struct HashTxSorter
{
    bool operator () (const TransactionFramePtr & tx1, const TransactionFramePtr & tx2)
    {
        // TODO.2 should we actually use ID here since multiple txs could have the same Hash
        return tx1->getHash() < tx2->getHash();
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
        Hash h1 = tx1->getHash(); // TODO.2 should we actually use ID here since multiple txs could have the same Hash
        Hash h2 = tx2->getHash(); 
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
        if(tx->getHash() < lastHash) return false;
        accountTxMap[tx->getSourceID()].push_back(tx);
        lastHash = tx->getHash();
    }

    for(auto item : accountTxMap)
    {
        TransactionFramePtr first;
        for(auto tx : item.second)
        {
            if(!first)
            {    
                if(!tx->loadAccount(app)) return false;
                if(tx->getSeqNum() != tx->getSourceAccount().getSeqNum() + 1) return false;
                if(tx->getSourceAccount().getBalance() < 
                    item.second.size() * app.getLedgerGateway().getTxFee()) return false;
            } else
            {
                tx->getSourceAccount() = first->getSourceAccount();
                if(tx->getSeqNum() != first->getSeqNum() + 1) return false;
            }
            first = tx;

            if(!tx->checkValid(app)) return false;
        }
    }
    return true;
}

uint256
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

/*
bool TransactionSet::operator > (const TransactionSet& other)
{
        if(mTransactions.size() > other.mTransactions.size()) return true;
        if(mTransactions.size() < other.mTransactions.size()) return false;
        if(getContentsHash() > other.getContentsHash()) return true;
        return false;
}
*/

TransactionFramePtr
TxSetFrame::getTransaction(uint256 const& txHash)
{
    for (auto tx : mTransactions)
    {
        if (txHash == tx->getHash())
            return (tx);
    }
    return (TransactionFramePtr());
}

// save this tx set to the node store in serialized format
void
TxSetFrame::store()
{
    // TODO.3
}

void
TxSetFrame::toXDR(TransactionSet& txSet)
{
    txSet.txs.resize(mTransactions.size());
    for (unsigned int n = 0; n < mTransactions.size(); n++)
    {
        txSet.txs[n]=mTransactions[n]->getEnvelope();
    }
}
}
