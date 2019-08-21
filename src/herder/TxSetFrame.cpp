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
#include <list>
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
        mTransactions.emplace_back(
            TransactionFrameBase::makeTransactionFromWire(networkID,
                                                          txEnvelope));
    }
    mPreviousLedgerHash = xdrSet.previousLedgerHash;
}

static bool
HashTxSorter(TransactionFrameBasePtr const& tx1,
             TransactionFrameBasePtr const& tx2)
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
    operator()(TransactionFrameBasePtr const& tx1,
               TransactionFrameBasePtr const& tx2) const
    {
        // need to use the hash of whole tx here since multiple txs could have
        // the same Contents
        return lessThanXored(tx1->getFullHash(), tx2->getFullHash(), mSetHash);
    }
};

static bool
SeqSorter(TransactionFrameBasePtr const& tx1,
          TransactionFrameBasePtr const& tx2)
{
    if (tx1->getSeqNum() != tx2->getSeqNum())
    {
        return tx1->getSeqNum() < tx2->getSeqNum();
    }

    // compare fee/minFee between tx1 and tx2
    auto v1 =
        bigMultiply(tx1->getFeeBid(), tx2->getOperationCountForValidation());
    auto v2 =
        bigMultiply(tx2->getFeeBid(), tx1->getOperationCountForValidation());
    if (v1 != v2)
    {
        return v1 > v2;
    }

    if (tx1->getInnerHash() < tx2->getInnerHash())
    {
        return tx1->getInnerHash() < tx2->getInnerHash();
    }

    return tx1->getFullHash() < tx2->getFullHash();
}

/*
    Build a list of transaction ready to be applied to the last closed ledger,
    based on the transaction set.

    The order satisfies:
    * transactions for an account are sorted by sequence number (ascending)
    * the order between accounts is randomized
*/
std::vector<TransactionFrameBasePtr>
TxSetFrame::sortForApply()
{
    auto txQueues = buildAccountTxQueues();

    // build txBatches
    // txBatches i-th element contains each i-th transaction for accounts with a
    // transaction in the transaction set
    std::list<AccountTransactionQueue> txBatches;

    while (!txQueues.empty())
    {
        txBatches.emplace_back();
        auto& curBatch = txBatches.back();
        // go over all users that still have transactions
        for (auto it = txQueues.begin(); it != txQueues.end();)
        {
            auto& h = it->second.front();
            curBatch.emplace_back(h);
            it->second.pop_front();
            if (it->second.empty())
            {
                // done with that user
                it = txQueues.erase(it);
            }
            else
            {
                it++;
            }
        }
    }

    std::unordered_set<Hash> includedTxs;
    std::vector<TransactionFrameBasePtr> retList;
    retList.reserve(mTransactions.size());
    for (auto& batch : txBatches)
    {
        // randomize each batch using the hash of the transaction set
        // as a way to randomize even more
        ApplyTxSorter s(getContentsHash());
        std::sort(batch.begin(), batch.end(), s);
        for (auto const& tx : batch)
        {
            for (auto const& applyTx : tx->transactionsToApply())
            {
                if (includedTxs.emplace(applyTx->getFullHash()).second)
                {
                    retList.emplace_back(applyTx);
                }
            }
        }
    }

    return retList;
}

struct FeeCompare
{
    Hash const mSeed;
    LedgerHeader const mHeader;

    FeeCompare(LedgerHeader const& header)
        : mSeed(HashUtils::random()), mHeader(header)
    {
    }

    bool
    operator()(TransactionFrameBasePtr const& tx1,
               TransactionFrameBasePtr const& tx2) const
    {
        // compare fee/minFee between tx1 and tx2
        auto v1 = bigMultiply(tx1->getFeeBid(),
                              tx2->getOperationCountForValidation());
        auto v2 = bigMultiply(tx2->getFeeBid(),
                              tx1->getOperationCountForValidation());
        if (v1 != v2)
        {
            return v1 < v2;
        }

        // use hash of transaction as a tie breaker
        return lessThanXored(tx1->getFullHash(), tx2->getFullHash(), mSeed);
    }
};

struct SurgeCompare
{
    FeeCompare const mFeeCompare;

    SurgeCompare(LedgerHeader const& header) : mFeeCompare(header)
    {
    }

    // return true if tx1 < tx2
    bool
    operator()(TxSetFrame::AccountTransactionQueue const* tx1,
               TxSetFrame::AccountTransactionQueue const* tx2) const
    {
        if (tx1 == nullptr || tx1->empty())
        {
            return tx2 ? !tx2->empty() : false;
        }
        if (tx2 == nullptr || tx2->empty())
        {
            return false;
        }

        return mFeeCompare(tx1->front(), tx2->front());
    }
};

std::unordered_map<AccountID, TxSetFrame::AccountTransactionQueue>
TxSetFrame::buildAccountTxQueues()
{
    std::unordered_map<AccountID, AccountTransactionQueue> actTxQueueMap;
    for (auto& tx : mTransactions)
    {
        auto id = tx->getSourceID();
        auto it = actTxQueueMap.find(id);
        if (it == actTxQueueMap.end())
        {
            auto d = std::make_pair(id, AccountTransactionQueue{});
            auto r = actTxQueueMap.insert(d);
            it = r.first;
        }
        it->second.emplace_back(tx);
    }

    for (auto& am : actTxQueueMap)
    {
        // sort each in sequence number order
        std::sort(am.second.begin(), am.second.end(), SeqSorter);
    }
    return actTxQueueMap;
}

void
TxSetFrame::surgePricingFilter(Application& app)
{
    LedgerTxn ltx(app.getLedgerTxnRoot());
    auto header = ltx.loadHeader();

    bool maxIsOps = header.current().ledgerVersion >= 11;

    size_t opsLeft;
    {
        size_t maxTxSetSize = header.current().maxTxSetSize;
        opsLeft = maxIsOps ? maxTxSetSize : (maxTxSetSize * MAX_OPS_PER_TX);
    }

    auto curSizeOps = maxIsOps ? sizeOp() : (sizeTx() * MAX_OPS_PER_TX);
    if (curSizeOps > opsLeft)
    {
        CLOG(WARNING, "Herder")
            << "surge pricing in effect! " << curSizeOps << " > " << opsLeft;

        auto actTxQueueMap = buildAccountTxQueues();

        SurgeCompare const surgeComp(header.current());
        std::priority_queue<AccountTransactionQueue*,
                            std::vector<AccountTransactionQueue*>, SurgeCompare>
            surgeQueue(surgeComp);

        std::priority_queue<TransactionFrameBasePtr,
                            std::vector<TransactionFrameBasePtr>, FeeCompare>
            feeBumpQueue(surgeComp.mFeeCompare);

        for (auto& am : actTxQueueMap)
        {
            surgeQueue.push(&am.second);
        }

        std::vector<TransactionFrameBasePtr> updatedSet;
        updatedSet.reserve(mTransactions.size());
        while (opsLeft >= 0 && (!surgeQueue.empty() || !feeBumpQueue.empty()))
        {
            if (feeBumpQueue.empty() ||
                (!surgeQueue.empty() &&
                 !surgeComp.mFeeCompare(surgeQueue.top()->front(),
                                        feeBumpQueue.top())))
            {
                auto cur = surgeQueue.top();
                surgeQueue.pop();
                // inspect the top candidate queue
                auto& curTopTx = cur->front();
                size_t opsCount =
                    maxIsOps ? curTopTx->getOperationCountForValidation()
                             : MAX_OPS_PER_TX;
                if (opsCount <= opsLeft)
                {
                    // pop from this one
                    int64_t seqNum = curTopTx->getSeqNum();
                    updatedSet.emplace_back(curTopTx);
                    opsLeft -= opsCount;
                    cur->pop_front();

                    while (!cur->empty() && cur->front()->getSeqNum() == seqNum)
                    {
                        feeBumpQueue.push(cur->front());
                        cur->pop_front();
                    }

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
            else
            {
                switch (feeBumpQueue.top()->getEnvelope().type())
                {
                case ENVELOPE_TYPE_TX_V0:
                    updatedSet.emplace_back(feeBumpQueue.top());
                    break;
                case ENVELOPE_TYPE_FEE_BUMP:
                    if (opsLeft > 0)
                    {
                        updatedSet.emplace_back(feeBumpQueue.top());
                        --opsLeft;
                    }
                    break;
                default:
                    throw std::runtime_error("Unexpected envelope type");
                }
                feeBumpQueue.pop();
            }
        }
        mTransactions = std::move(updatedSet);
        sortForHash();
    }
}

static std::vector<TransactionFrameBasePtr>
dropDuplicates(std::vector<TransactionFrameBasePtr> const& input,
               std::vector<TransactionFrameBasePtr>& dropped)
{
    auto iter = input.begin();
    if (iter == input.end())
    {
        return {};
    }

    std::vector<TransactionFrameBasePtr> res{*iter};
    while (++iter != input.end())
    {
        if ((*iter)->getFullHash() == res.back()->getFullHash())
        {
            dropped.emplace_back(*iter);
        }
        else
        {
            res.emplace_back(*iter);
        }
    }
    return res;
}

bool
TxSetFrame::checkOrTrim(Application& app, bool onlyCheck,
                        std::vector<TransactionFrameBasePtr>& trimmed)
{
    LedgerTxn ltx(app.getLedgerTxnRoot());

    if (!std::is_sorted(mTransactions.begin(), mTransactions.end(),
                        HashTxSorter))
    {
        CLOG(DEBUG, "Herder") << "Got bad txSet: not sorted correctly";
        return false;
    }

    auto transactions = dropDuplicates(mTransactions, trimmed);
    if (onlyCheck && transactions.size() != mTransactions.size())
    {
        CLOG(DEBUG, "Herder") << "Got bad txSet: contains duplicate txs";
        return false;
    }

    auto accountTxMap = buildAccountTxQueues();
    std::unordered_map<AccountID, int64_t> accountFeeMap;
    for (auto& item : accountTxMap)
    {
        TransactionFrameBasePtr lastTx;
        for (auto iter = item.second.begin(); iter != item.second.end(); ++iter)
        {
            auto& tx = *iter;

            int64_t lastSeq = 0;
            if (lastTx)
            {
                if (lastTx->getSeqNum() == tx->getSeqNum())
                {
                    if (lastTx->getInnerHash() != tx->getInnerHash())
                    {
                        if (onlyCheck)
                        {
                            CLOG(DEBUG, "Herder") << "Got bad txSet: contains "
                                                     "conflicting inner tx";
                            return false;
                        }
                        trimmed.emplace_back(tx);
                        continue;
                    }
                    lastSeq = lastTx->getSeqNum() - 1;
                }
                else
                {
                    lastSeq = lastTx->getSeqNum();
                }
            }

            if (!tx->checkValid(ltx, lastSeq))
            {
                if (onlyCheck)
                {
                    CLOG(DEBUG, "Herder")
                        << "Got bad txSet: contains invalid tx"
                        << " got: " << xdr::xdr_to_string(tx->getEnvelope())
                        << " result: " << xdr::xdr_to_string(tx->getResult());
                    return false;
                }
                trimmed.emplace_back(tx);
                continue;
            }

            int64_t& accFee = accountFeeMap[tx->getFeeSourceID()];
            if (INT64_MAX - accFee < tx->getFeeBid())
            {
                accFee = INT64_MAX;
            }
            else
            {
                accFee += tx->getFeeBid();
            }
            lastTx = tx;
        }
    }

    for (auto& item : accountTxMap)
    {
        int64_t lastSeq =
            item.second.empty() ? 0 : (item.second.front()->getSeqNum() - 1);
        for (auto iter = item.second.begin(); iter != item.second.end(); ++iter)
        {
            auto& tx = *iter;
            if (tx->getSeqNum() - lastSeq > 1)
            {
                // This can only be reached if a transaction has already been
                // trimmed in this loop, so there is no need to specially handle
                // onlyCheck.
                trimmed.insert(trimmed.end(), iter, item.second.end());
                break;
            }

            auto feeSource = stellar::loadAccount(ltx, tx->getFeeSourceID());
            if (!feeSource || getAvailableBalance(ltx.loadHeader(), feeSource) <
                                  accountFeeMap[tx->getFeeSourceID()])
            {
                if (onlyCheck)
                {
                    return false;
                }
                trimmed.emplace_back(tx);
                continue;
            }

            lastSeq = tx->getSeqNum();
        }
    }

    for (auto& tx : trimmed)
    {
        removeTx(tx);
    }
    return true;
}

std::vector<TransactionFrameBasePtr>
TxSetFrame::trimInvalid(Application& app)
{
    sortForHash();
    std::vector<TransactionFrameBasePtr> trimmed;
    checkOrTrim(app, false, trimmed);
    return trimmed;
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
        CLOG(DEBUG, "Herder") << "Got bad txSet: wrong previous ledger hash"
                              << " got: " << hexAbbrev(mPreviousLedgerHash)
                              << " expected: " << hexAbbrev(lcl.hash);
        return false;
    }

    if (this->size(lcl.header) > lcl.header.maxTxSetSize)
    {
        CLOG(DEBUG, "Herder") << "Got bad txSet: too many txs"
                              << " got: " << this->size(lcl.header)
                              << " limit: " << lcl.header.maxTxSetSize;
        return false;
    }

    std::vector<TransactionFrameBasePtr> trimmed;
    return checkOrTrim(app, true, trimmed);
}

void
TxSetFrame::removeTx(TransactionFrameBasePtr tx)
{
    auto it = std::find(mTransactions.begin(), mTransactions.end(), tx);
    if (it != mTransactions.end())
        mTransactions.erase(it);
    mHashIsValid = false;
}

Hash const&
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
    size_t res = 0;
    std::unordered_set<Hash> includedInnerTx;
    for (auto const& tx : mTransactions)
    {
        if (includedInnerTx.insert(tx->getInnerHash()).second)
        {
            res += tx->getOperationCountForValidation();
        }
        else if (tx->getEnvelope().type() == ENVELOPE_TYPE_FEE_BUMP)
        {
            res += 1;
        }
    }
    return res;
}

int64_t
TxSetFrame::getBaseFee(LedgerHeader const& lh) const
{
    int64_t baseFee = lh.baseFee;
    if (lh.ledgerVersion >= 11)
    {
        size_t ops = 0;
        int64_t lowBaseFee = std::numeric_limits<int64_t>::max();
        for (auto& txPtr : mTransactions)
        {
            auto txOps = txPtr->getOperationCountForValidation();
            ops += txOps;
            int64_t txBaseFee =
                bigDivide(txPtr->getFeeBid(), 1, static_cast<int64_t>(txOps),
                          Rounding::ROUND_UP);
            lowBaseFee = std::min(lowBaseFee, txBaseFee);
        }
        // if surge pricing was in action, use the lowest base fee bid from the
        // transaction set
        size_t surgeOpsCutoff = 0;
        if (lh.maxTxSetSize >= MAX_OPS_PER_TX)
        {
            surgeOpsCutoff = lh.maxTxSetSize - MAX_OPS_PER_TX;
        }
        if (ops > surgeOpsCutoff)
        {
            baseFee = lowBaseFee;
        }
    }
    return baseFee;
}

int64_t
TxSetFrame::getTotalFees(LedgerHeader const& lh) const
{
    auto baseFee = getBaseFee(lh);
    return std::accumulate(mTransactions.begin(), mTransactions.end(),
                           int64_t(0),
                           [&](int64_t t, TransactionFrameBasePtr const& tx) {
                               return t + tx->getFee(lh, baseFee);
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
