// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "TxSetFrame.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "database/Database.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "main/Application.h"
#include "main/Config.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include "util/XDROperators.h"
#include "xdrpp/marshal.h"
#include <algorithm>

#include "xdrpp/printer.h"

namespace stellar
{

using namespace std;

TxSetFrame::TxSetFrame(Hash const& previousLedgerHash)
    : mHashIsValid(false), mPreviousLedgerHash(previousLedgerHash)
{
}

TxSetFrame::TxSetFrame(Hash const& networkID, TransactionSet const& xdrSet)
    : mHashIsValid(false)
{
    for (auto const& txEnvelope : xdrSet.txs)
    {
        TransactionFramePtr tx =
            TransactionFrame::makeTransactionFromWire(networkID, txEnvelope);
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
    Hash mSetHash;
    ApplyTxSorter(Hash h) : mSetHash{std::move(h)}
    {
    }

    bool
    operator()(TransactionFramePtr const& tx1,
               TransactionFramePtr const& tx2) const
    {
        // need to use the hash of whole tx here since multiple txs could have
        // the same Contents
        return lessThanXored(tx1->getFullHash(), tx2->getFullHash(), mSetHash);
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
    map<AccountID, size_t> accountTxCountMap;
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
    map<AccountID, double>& mAccountFeeMap;
    SurgeSorter(map<AccountID, double>& afm) : mAccountFeeMap(afm)
    {
    }

    bool
    operator()(TransactionFramePtr const& tx1, TransactionFramePtr const& tx2)
    {
        if (tx1->getSourceID() == tx2->getSourceID())
            return tx1->getSeqNum() < tx2->getSeqNum();
        double fee1 = mAccountFeeMap[tx1->getSourceID()];
        double fee2 = mAccountFeeMap[tx2->getSourceID()];
        if (fee1 == fee2)
            return tx1->getSourceID() < tx2->getSourceID();
        return fee1 > fee2;
    }
};

void
TxSetFrame::surgePricingFilter(Application& app)
{
    LedgerTxn ltx(app.getLedgerTxnRoot());
    auto header = ltx.loadHeader();
    size_t max = header.current().maxTxSetSize;
    if (mTransactions.size() > max)
    { // surge pricing in effect!
        CLOG(WARNING, "Herder")
            << "surge pricing in effect! " << mTransactions.size();

        // determine the fee ratio for each account
        map<AccountID, double> accountFeeMap;
        for (auto& tx : mTransactions)
        {
            double fee = tx->getFee();
            double minFee = (double)tx->getMinFee(header);
            double r = fee / minFee;

            double now = accountFeeMap[tx->getSourceID()];
            if (now == 0)
                accountFeeMap[tx->getSourceID()] = r;
            else if (r < now)
                accountFeeMap[tx->getSourceID()] = r;
        }

        // sort tx by amount of fee they have paid
        // remove the bottom that aren't paying enough
        std::vector<TransactionFramePtr> tempList = mTransactions;
        std::sort(tempList.begin(), tempList.end(), SurgeSorter(accountFeeMap));

        for (auto iter = tempList.begin() + max; iter != tempList.end(); iter++)
        {
            removeTx(*iter);
        }
    }
}

bool
TxSetFrame::checkOrTrim(
    Application& app,
    std::function<bool(TransactionFramePtr, SequenceNumber)>
        processInvalidTxLambda,
    std::function<bool(std::vector<TransactionFramePtr> const&)>
        processInsufficientBalance)
{
    LedgerTxn ltx(app.getLedgerTxnRoot());

    map<AccountID, vector<TransactionFramePtr>> accountTxMap;

    Hash lastHash;
    for (auto& tx : mTransactions)
    {
        if (tx->getFullHash() < lastHash)
        {
            CLOG(DEBUG, "Herder")
                << "bad txSet: " << hexAbbrev(mPreviousLedgerHash)
                << " not sorted correctly";
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
            if (!tx->checkValid(app, ltx, lastSeq))
            {
                if (processInvalidTxLambda(tx, lastSeq))
                    continue;

                return false;
            }
            totFee += tx->getFee();

            lastTx = tx;
            lastSeq = tx->getSeqNum();
        }
        if (lastTx)
        {
            // make sure account can pay the fee for all these tx
            auto const& source =
                stellar::loadAccount(ltx, lastTx->getSourceID());
            if (getAvailableBalance(ltx.loadHeader(), source) < totFee)
            {
                if (!processInsufficientBalance(item.second))
                    return false;
            }
        }
    }

    return true;
}

void
TxSetFrame::trimInvalid(Application& app,
                        std::vector<TransactionFramePtr>& trimmed)
{
    sortForHash();

    auto processInvalidTxLambda = [&](TransactionFramePtr tx,
                                      SequenceNumber lastSeq) {
        trimmed.push_back(tx);
        removeTx(tx);
        return true;
    };
    auto processInsufficientBalance =
        [&](vector<TransactionFramePtr> const& item) {
            for (auto& tx : item)
            {
                trimmed.push_back(tx);
                removeTx(tx);
            }
            return true;
        };

    checkOrTrim(app, processInvalidTxLambda, processInsufficientBalance);
}

// need to make sure every account that is submitting a tx has enough to pay
// the fees of all the tx it has submitted in this set
// check seq num
bool
TxSetFrame::checkValid(Application& app)
{
    auto& lcl = app.getLedgerManager().getLastClosedLedgerHeader();
    // Start by checking previousLedgerHash
    if (lcl.hash != mPreviousLedgerHash)
    {
        CLOG(DEBUG, "Herder")
            << "Got bad txSet: " << hexAbbrev(mPreviousLedgerHash)
            << " ; expected: " << hexAbbrev(lcl.hash);
        return false;
    }

    if (mTransactions.size() > lcl.header.maxTxSetSize)
    {
        CLOG(DEBUG, "Herder")
            << "Got bad txSet: too many txs " << mTransactions.size() << " > "
            << lcl.header.maxTxSetSize;
        return false;
    }

    auto processInvalidTxLambda = [&](TransactionFramePtr tx,
                                      SequenceNumber const& lastSeq) {
        CLOG(DEBUG, "Herder")
            << "bad txSet: " << hexAbbrev(mPreviousLedgerHash) << " tx invalid"
            << " lastSeq:" << lastSeq
            << " tx: " << xdr::xdr_to_string(tx->getEnvelope())
            << " result: " << tx->getResultCode();

        return false;
    };
    auto processInsufficientBalance =
        [&](vector<TransactionFramePtr> const& item) {
            CLOG(DEBUG, "Herder")
                << "bad txSet: " << hexAbbrev(mPreviousLedgerHash)
                << " account can't pay fee"
                << " tx:" << xdr::xdr_to_string(item.back()->getEnvelope());

            return false;
        };
    return checkOrTrim(app, processInvalidTxLambda, processInsufficientBalance);
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
} // namespace stellar
