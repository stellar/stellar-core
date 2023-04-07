// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/TxAdvertQueue.h"
#include "ledger/LedgerManager.h"

namespace stellar
{

constexpr uint32 const ADVERT_CACHE_SIZE = 50000;

TxAdvertQueue::TxAdvertQueue(Application& app) : mApp(app) , mAdvertHistory(ADVERT_CACHE_SIZE)
{
}

bool
TxAdvertQueue::peerKnowsHash(Hash const& hash)
{
    return mAdvertHistory.exists(hash);
}

void
TxAdvertQueue::rememberHash(Hash const& hash, uint32_t ledgerSeq)
{
    mAdvertHistory.put(hash, ledgerSeq);
}


size_t
TxAdvertQueue::size() const
{
    return mIncomingTxHashes.size() + mTxHashesToRetry.size();
}

void
TxAdvertQueue::appendHashesToRetryAndMaybeTrim(std::list<Hash>& list)
{
    mTxHashesToRetry.splice(mTxHashesToRetry.end(), list);
    while (size() > mApp.getLedgerManager().getLastMaxTxSetSizeOps())
    {
        pop();
    }
}

void
TxAdvertQueue::queueAndMaybeTrim(TxAdvertVector const& txHashes, uint32_t seq)
{
    for (auto const& hash : txHashes)
    {
        rememberHash(hash, seq);
    }

    auto it = txHashes.begin();
    size_t const limit = mApp.getLedgerManager().getLastMaxTxSetSizeOps();
    if (txHashes.size() > limit)
    {
        // If txHashes has more than getLastMaxTxSetSizeOps txns, then
        // the first (txHashes.size() - getLastMaxTxSetSizeOps) txns will be
        // popped in the while loop below. Therefore, we won't even bother
        // pushing them.
        it += txHashes.size() - limit;
    }

    auto now = mApp.getClock().now();
    while (it != txHashes.end())
    {
        mIncomingTxHashes.emplace_back(*it, now);
        it++;
    }

    while (size() > limit)
    {
        pop();
    }
}

std::pair<Hash, std::optional<VirtualClock::time_point>>
TxAdvertQueue::pop()
{
    releaseAssert(size() > 0);

    if (mTxHashesToRetry.size() > 0)
    {
        auto const h = mTxHashesToRetry.front();
        mTxHashesToRetry.pop_front();
        return std::make_pair(h, std::nullopt);
    }
    else
    {
        auto const h = mIncomingTxHashes.front();
        mIncomingTxHashes.pop_front();
        return std::make_pair(
            h.first, std::make_optional<VirtualClock::time_point>(h.second));
    }
}

void 
TxAdvertQueue::clearBelow(uint32_t ledgerSeq)
{
    mAdvertHistory.erase_if(
        [&](uint32_t const& seq) { return seq < ledgerSeq; });
}

}
