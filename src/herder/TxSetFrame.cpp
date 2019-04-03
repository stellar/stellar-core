// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "TxSetFrame.h"
#include "crypto/Hex.h"
#include "crypto/Random.h"
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
#include <numeric>

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

using AccountTransactionQueue = std::deque<TransactionFramePtr>;

struct SurgeCompare
{
    Hash mSeed;
    LedgerTxnHeader const& mHeader;
    SurgeCompare(LedgerTxnHeader const& header)
        : mSeed(HashUtils::random()), mHeader(header)
    {
    }

    // return true if tx1 < tx2
    bool
    operator()(AccountTransactionQueue const* tx1,
               AccountTransactionQueue const* tx2) const
    {
        if (tx1 == nullptr || tx1->empty())
        {
            return tx2 ? !tx2->empty() : false;
        }
        if (tx2 == nullptr || tx2->empty())
        {
            return false;
        }

        auto& top1 = tx1->front();
        auto& top2 = tx2->front();

        // compare fee/minFee between top1 and top2
        auto v1 = bigMultiply(top1->getFee(), top2->getMinFee(mHeader));
        auto v2 = bigMultiply(top2->getFee(), top1->getMinFee(mHeader));
        if (v1 < v2)
        {
            return true;
        }
        else if (v1 > v2)
        {
            return false;
        }
        // use hash of transaction as a tie breaker
        return lessThanXored(top1->getFullHash(), top2->getFullHash(), mSeed);
    }
};

void
TxSetFrame::surgePricingFilter(Application& app)
{
    LedgerTxn ltx(app.getLedgerTxnRoot());
    auto header = ltx.loadHeader();

    bool maxIsOps = header.current().ledgerVersion >= 11;

    auto maxSize = header.current().maxTxSetSize;
    auto curSize = size(header.current());
    if (curSize > maxSize)
    {
        CLOG(WARNING, "Herder")
            << "surge pricing in effect! " << curSize << " > " << maxSize;
        std::unordered_map<AccountID, AccountTransactionQueue> actTxQueueMap;
        for (auto& tx : mTransactions)
        {
            if (tx->getOperations().size() == 0)
            {
                // ensure that "checkValid" was called
                throw std::runtime_error(
                    "Surge pricing should not be run on invalid transactions");
            }
            auto& id = tx->getSourceID();
            auto it = actTxQueueMap.find(id);
            if (it == actTxQueueMap.end())
            {
                auto d = std::make_pair(id, AccountTransactionQueue{});
                auto r = actTxQueueMap.insert(d);
                it = r.first;
            }
            it->second.emplace_back(tx);
        }

        SurgeCompare const surge(header);
        std::priority_queue<AccountTransactionQueue*,
                            std::vector<AccountTransactionQueue*>, SurgeCompare>
            surgeQueue(surge);

        for (auto& am : actTxQueueMap)
        {
            // sort each in sequence number order
            std::sort(am.second.begin(), am.second.end(), SeqSorter);
            surgeQueue.push(&am.second);
        }

        size_t opsLeft = maxIsOps ? maxSize : (maxSize * MAX_OPS_PER_TX);
        std::vector<TransactionFramePtr> updatedSet;
        updatedSet.reserve(mTransactions.size());
        while (opsLeft > 0 && !surgeQueue.empty())
        {
            auto cur = surgeQueue.top();
            surgeQueue.pop();
            // inspect the top candidate queue
            auto& curTopTx = cur->front();
            size_t opsCount =
                maxIsOps ? curTopTx->getOperations().size() : MAX_OPS_PER_TX;
            if (opsCount <= opsLeft)
            {
                // pop from this one
                updatedSet.emplace_back(curTopTx);
                cur->pop_front();
                opsLeft -= opsCount;
                // if there are more transactions, put it back
                if (!cur->empty())
                {
                    surgeQueue.push(cur);
                }
            }
            else
            {
                // drop this transaction -> we need to drop the others
                cur->clear();
            }
        }
        mTransactions = std::move(updatedSet);
        sortForHash();
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

    if (this->size(lcl.header) > lcl.header.maxTxSetSize)
    {
        CLOG(DEBUG, "Herder")
            << "Got bad txSet: too many txs " << this->size(lcl.header) << " > "
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

size_t
TxSetFrame::size(LedgerHeader const& lh) const
{
    return lh.ledgerVersion >= 11 ? sizeOp() : sizeTx();
}

size_t
TxSetFrame::sizeOp() const
{
    return std::accumulate(
        mTransactions.begin(), mTransactions.end(), size_t(0),
        [](size_t a, TransactionFramePtr const& tx) {
            return a + tx->getEnvelope().tx.operations.size();
        });
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
