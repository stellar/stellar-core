// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "TxSetFrame.h"
#include "xdrpp/marshal.h"
#include "crypto/SHA.h"
#include "util/Logging.h"
#include "crypto/Hex.h"
#include "main/Application.h"
#include "main/Config.h"
#include <algorithm>

namespace stellar
{

using namespace std;

TxSetFrame::TxSetFrame(Hash const& previousLedgerHash)
    : mHashIsValid(false), mPreviousLedgerHash(previousLedgerHash)
{
}

TxSetFrame::TxSetFrame(TransactionSet const& xdrSet) : mHashIsValid(false)
{
    for (auto const& txEnvelope : xdrSet.txs)
    {
        TransactionFramePtr tx =
            TransactionFrame::makeTransactionFromWire(txEnvelope);
        mTransactions.push_back(tx);
    }
    mPreviousLedgerHash = xdrSet.previousLedgerHash;
}

static bool
HashTxSorter(TransactionFramePtr const& tx1, TransactionFramePtr const& tx2)
{
    // need to use the hash of whole tx here since multiple txs could have
    // the same Contents
    return tx1->getFullHash() < tx2->getFullHash();
}

// order the txset correctly
// must take into account multiple tx from same account
void
TxSetFrame::sortForHash()
{
    std::sort(mTransactions.begin(), mTransactions.end(), HashTxSorter);
    mHashIsValid = false;
}

// We want to XOR the tx hash with the set hash.
// This way people can't predict the order that txs will be applied in
struct ApplyTxSorter
{
    Hash const& mSetHash;
    ApplyTxSorter(Hash const& h) : mSetHash(h)
    {
    }

    bool operator()(TransactionFramePtr const& tx1,
                    TransactionFramePtr const& tx2) const
    {
        // need to use the hash of whole tx here since multiple txs could have
        // the same Contents
        Hash h1 = tx1->getFullHash();
        Hash h2 = tx2->getFullHash();
        Hash v1, v2;
        for (int n = 0; n < 32; n++)
        {
            v1[n] = mSetHash[n] ^ h1[n];
            v2[n] = mSetHash[n] ^ h2[n];
        }

        return v1 < v2;
    }
};

static bool
SeqSorter(TransactionFramePtr const& tx1, TransactionFramePtr const& tx2)
{
    return tx1->getSeqNum() < tx2->getSeqNum();
}

/*
    Build a list of transaction ready to be applied to the last closed ledger,
    based on the transaction set.

    The order satisfies:
    * transactions for an account are sorted by sequence number (ascending)
    * the order between accounts is randomized
*/
std::vector<TransactionFramePtr>
TxSetFrame::sortForApply()
{
    vector<TransactionFramePtr> retList;

    vector<vector<TransactionFramePtr>> txBatches(4);
    map<uint256, size_t> accountTxCountMap;
    retList = mTransactions;
    // sort all the txs by seqnum
    std::sort(retList.begin(), retList.end(), SeqSorter);

    // build the txBatches
    // batch[i] contains the i-th transaction for any account with
    // a transaction in the transaction set
    for (auto tx : retList)
    {
        auto& v = accountTxCountMap[tx->getSourceID()];

        if (v >= txBatches.size())
        {
            txBatches.resize(v + 4);
        }
        txBatches[v].push_back(tx);
        v++;
    }

    retList.clear();

    for (auto& batch : txBatches)
    {
        // randomize each batch using the hash of the transaction set
        // as a way to randomize even more
        ApplyTxSorter s(getContentsHash());
        std::sort(batch.begin(), batch.end(), s);
        for (auto tx : batch)
        {
            retList.push_back(tx);
        }
    }

    return retList;
}

struct SurgeSorter 
{
    Application& mApp;
    SurgeSorter(Application& app) : mApp(app) {}
    bool operator () (TransactionFramePtr const& tx1, TransactionFramePtr const& tx2)
    {
        return tx1->getFeeRatio(mApp) < tx2->getFeeRatio(mApp);
    }
};


void 
TxSetFrame::surgePricingFilter(Application& app)
{
    int max = app.getConfig().DESIRED_MAX_TX_PER_LEDGER;
    if(mTransactions.size() > max)
    {  // surge pricing in effect!
        CLOG(DEBUG, "Herder") << "surge pricing in effect! " << mTransactions.size();
        //sort tx by amount of fee they have paid 
        // remove the bottom that aren't paying enough
        std::vector<TransactionFramePtr> tempList = mTransactions;
        std::sort(tempList.begin(), tempList.end(), SurgeSorter(app) );

        for(auto iter = tempList.begin() + max; iter != tempList.end(); iter++)
        {
            removeTx(*iter);
        }
    }
}

// TODO.3 this and checkValid share a lot of code
void
TxSetFrame::trimInvalid(Application& app,
                        std::vector<TransactionFramePtr> trimmed)
{
    sortForHash();

    using xdr::operator==;

    map<uint256, vector<TransactionFramePtr>> accountTxMap;

    Hash lastHash;
    for (auto tx : mTransactions)
    {
        accountTxMap[tx->getSourceID()].push_back(tx);
        lastHash = tx->getFullHash();
    }

    for (auto& item : accountTxMap)
    {
        // order by sequence number
        std::sort(item.second.begin(), item.second.end(), SeqSorter);

        TransactionFramePtr lastTx;
        SequenceNumber lastSeq = 0;
        int64_t totFee = 0;
        for (auto& tx : item.second)
        {
            if (!tx->checkValid(app, lastSeq))
            {
                trimmed.push_back(tx);
                removeTx(tx);
                continue;
            }
            totFee += tx->getFee();

            lastTx = tx;
            lastSeq = tx->getSeqNum();
        }
        if (lastTx)
        {
            // make sure account can pay the fee for all these tx
            int64_t newBalance =
                lastTx->getSourceAccount().getBalance() - totFee;
            if (newBalance < lastTx->getSourceAccount().getMinimumBalance(
                                 app.getLedgerManager()))
            {
                for (auto& tx : item.second)
                {
                    trimmed.push_back(tx);
                    removeTx(tx);
                }
            }
        }
    }
}

// need to make sure every account that is submitting a tx has enough to pay
// the fees of all the tx it has submitted in this set
// check seq num
bool
TxSetFrame::checkValid(Application& app) const
{
    using xdr::operator==;

    // Start by checking previousLedgerHash
    if (app.getLedgerManager().getLastClosedLedgerHeader().hash !=
        mPreviousLedgerHash)
    {
        CLOG(TRACE, "Herder")
            << "Got bad txSet: " << hexAbbrev(mPreviousLedgerHash)
            << " ; expected: "
            << hexAbbrev(
                   app.getLedgerManager().getLastClosedLedgerHeader().hash);
        return false;
    }

    map<uint256, vector<TransactionFramePtr>> accountTxMap;

    Hash lastHash;
    for (auto tx : mTransactions)
    {
        // make sure the set is sorted correctly
        if (tx->getFullHash() < lastHash)
        {
            return false;
        }
        accountTxMap[tx->getSourceID()].push_back(tx);
        lastHash = tx->getFullHash();
    }

    for (auto& item : accountTxMap)
    {
        // order by sequence number
        std::sort(item.second.begin(), item.second.end(), SeqSorter);

        TransactionFramePtr lastTx;
        SequenceNumber lastSeq = 0;
        int64_t totFee = 0;
        for (auto& tx : item.second)
        {
            if (!tx->checkValid(app, lastSeq))
            {
                return false;
            }
            totFee += tx->getFee();

            lastTx = tx;
            lastSeq = tx->getSeqNum();
        }
        if (lastTx)
        {
            // make sure account can pay the fee for all these tx
            int64_t newBalance =
                lastTx->getSourceAccount().getBalance() - totFee;
            if (newBalance < lastTx->getSourceAccount().getMinimumBalance(
                                 app.getLedgerManager()))
            {
                return false;
            }
        }
    }
    return true;
}

void
TxSetFrame::removeTx(TransactionFramePtr tx)
{
    auto it = std::find(mTransactions.begin(), mTransactions.end(), tx);
    if (it != mTransactions.end())
        mTransactions.erase(it);
    mHashIsValid = false;
}

Hash
TxSetFrame::getContentsHash()
{
    if (!mHashIsValid)
    {
        sortForHash();
        auto hasher = SHA256::create();
        hasher->add(mPreviousLedgerHash);
        for (unsigned int n = 0; n < mTransactions.size(); n++)
        {
            hasher->add(xdr::xdr_to_opaque(mTransactions[n]->getEnvelope()));
        }
        mHash = hasher->finish();
        mHashIsValid = true;
    }
    return mHash;
}

Hash&
TxSetFrame::previousLedgerHash()
{
    mHashIsValid = false;
    return mPreviousLedgerHash;
}

Hash const&
TxSetFrame::previousLedgerHash() const
{
    return mPreviousLedgerHash;
}

void
TxSetFrame::toXDR(TransactionSet& txSet)
{
    txSet.txs.resize(xdr::size32(mTransactions.size()));
    for (unsigned int n = 0; n < mTransactions.size(); n++)
    {
        txSet.txs[n] = mTransactions[n]->getEnvelope();
    }
    txSet.previousLedgerHash = mPreviousLedgerHash;
}
}
