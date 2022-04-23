// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "TxSetFrame.h"
#include "TxSetUtils.h"
#include "crypto/Hex.h"
#include "crypto/Random.h"
#include "crypto/SHA.h"
#include "database/Database.h"
#include "herder/SurgePricingUtils.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "main/Application.h"
#include "main/Config.h"
#include "transactions/TransactionUtils.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/ProtocolVersion.h"
#include "util/XDRCereal.h"
#include "util/XDROperators.h"
#include "xdrpp/marshal.h"

#include <Tracy.hpp>
#include <algorithm>
#include <list>
#include <numeric>

namespace stellar
{

using namespace std;

TxSetFrame::TxSetFrame(Hash const& previousLedgerHash,
                       Transactions const& transactions)
    : mPreviousLedgerHash(previousLedgerHash)
    , mTxsInHashOrder(TxSetUtils::sortTxsInHashOrder(transactions))
    , mHash(
          TxSetUtils::computeContentsHash(previousLedgerHash, mTxsInHashOrder))
{
}

TxSetFrame::TxSetFrame(Hash const& previousLedgerHash)
    : TxSetFrame(previousLedgerHash, Transactions{})
{
}

TxSetFrame::TxSetFrame(Hash const& networkID, TransactionSet const& xdrSet)
    : mPreviousLedgerHash(xdrSet.previousLedgerHash)
    , mTxsInHashOrder(TxSetUtils::sortTxsInHashOrder(networkID, xdrSet))
    , mHash(
          TxSetUtils::computeContentsHash(mPreviousLedgerHash, mTxsInHashOrder))
{
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

/*
    Build a list of transaction ready to be applied to the last closed ledger,
    based on the transaction set.

    The order satisfies:
    * transactions for an account are sorted by sequence number (ascending)
    * the order between accounts is randomized
*/
TxSetFrame::Transactions
TxSetFrame::getTxsInApplyOrder() const
{
    ZoneScoped;
    auto txQueues = TxSetUtils::buildAccountTxQueues(*this);

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

    Transactions txsInApplyOrder;
    txsInApplyOrder.reserve(mTxsInHashOrder.size());
    for (auto& batch : txBatches)
    {
        // randomize each batch using the hash of the transaction set
        // as a way to randomize even more
        ApplyTxSorter s(getContentsHash());
        std::sort(batch.begin(), batch.end(), s);
        for (auto const& tx : batch)
        {
            txsInApplyOrder.push_back(tx);
        }
    }
    return txsInApplyOrder;
}

// need to make sure every account that is submitting a tx has enough to pay
// the fees of all the tx it has submitted in this set
// check seq num
bool
TxSetFrame::checkValid(Application& app, uint64_t lowerBoundCloseTimeOffset,
                       uint64_t upperBoundCloseTimeOffset) const
{
    ZoneScoped;
    auto& lcl = app.getLedgerManager().getLastClosedLedgerHeader();

    // Start by checking previousLedgerHash
    if (lcl.hash != mPreviousLedgerHash)
    {
        CLOG_DEBUG(Herder, "Got bad txSet: {}, expected {}",
                   hexAbbrev(mPreviousLedgerHash), hexAbbrev(lcl.hash));
        return false;
    }

    if (this->size(lcl.header) > lcl.header.maxTxSetSize)
    {
        CLOG_DEBUG(Herder, "Got bad txSet: too many txs {} > {}",
                   this->size(lcl.header), lcl.header.maxTxSetSize);
        return false;
    }

    if (!std::is_sorted(mTxsInHashOrder.begin(), mTxsInHashOrder.end(),
                        [](auto const& lhs, auto const& rhs) {
                            return lhs->getFullHash() < rhs->getFullHash();
                        }))
    {
        CLOG_DEBUG(Herder, "Got bad txSet: {} not sorted correctly",
                   hexAbbrev(mPreviousLedgerHash));
        return false;
    }

    return TxSetUtils::getInvalidTxList(app, *this, lowerBoundCloseTimeOffset,
                                        upperBoundCloseTimeOffset, true)
        .empty();
}

size_t
TxSetFrame::size(LedgerHeader const& lh) const
{
    return protocolVersionStartsFrom(lh.ledgerVersion, ProtocolVersion::V_11)
               ? sizeOp()
               : sizeTx();
}

size_t
TxSetFrame::sizeOp() const
{
    ZoneScoped;
    return std::accumulate(mTxsInHashOrder.begin(), mTxsInHashOrder.end(),
                           size_t(0),
                           [](size_t a, TransactionFrameBasePtr const& tx) {
                               return a + tx->getNumOperations();
                           });
}

int64_t
TxSetFrame::getBaseFee(LedgerHeader const& lh) const
{
    int64_t baseFee = lh.baseFee;
    if (protocolVersionStartsFrom(lh.ledgerVersion, ProtocolVersion::V_11))
    {
        size_t ops = 0;
        int64_t lowBaseFee = std::numeric_limits<int64_t>::max();
        for (auto& txPtr : mTxsInHashOrder)
        {
            auto txOps = txPtr->getNumOperations();
            ops += txOps;
            int64_t txBaseFee = bigDivideOrThrow(txPtr->getFeeBid(), 1,
                                                 static_cast<int64_t>(txOps),
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
    ZoneScoped;
    auto baseFee = getBaseFee(lh);
    return std::accumulate(mTxsInHashOrder.begin(), mTxsInHashOrder.end(),
                           int64_t(0),
                           [&](int64_t t, TransactionFrameBasePtr const& tx) {
                               return t + tx->getFee(lh, baseFee, true);
                           });
}

void
TxSetFrame::toXDR(TransactionSet& txSet) const
{
    ZoneScoped;
    releaseAssert(std::is_sorted(mTxsInHashOrder.begin(), mTxsInHashOrder.end(),
                                 TxSetUtils::HashTxSorter));
    txSet.txs.resize(xdr::size32(mTxsInHashOrder.size()));
    for (unsigned int n = 0; n < mTxsInHashOrder.size(); n++)
    {
        txSet.txs[n] = mTxsInHashOrder[n]->getEnvelope();
    }
    txSet.previousLedgerHash = mPreviousLedgerHash;
}

} // namespace stellar
