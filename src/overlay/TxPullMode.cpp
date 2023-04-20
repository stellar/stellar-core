// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/TxPullMode.h"
#include "crypto/Hex.h"
#include "ledger/LedgerManager.h"
#include "overlay/Peer.h"
#include "util/Logging.h"

namespace stellar
{

constexpr uint32 const ADVERT_CACHE_SIZE = 50000;

TxPullMode::TxPullMode(Application& app, std::weak_ptr<Peer> peer)
    : mApp(app)
    , mAdvertHistory(ADVERT_CACHE_SIZE)
    , mAdvertTimer(app)
    , mWeakPeer(peer)
{
}

void
TxPullMode::flushAdvert()
{
    if (mOutgoingTxHashes.size() > 0)
    {
        StellarMessage adv;
        adv.type(FLOOD_ADVERT);

        adv.floodAdvert().txHashes = std::move(mOutgoingTxHashes);
        auto msg = std::make_shared<StellarMessage>(adv);
        mOutgoingTxHashes.clear();
        mApp.postOnMainThread(
            [weak = mWeakPeer, msg = std::move(msg)]() {
                auto strong = weak.lock();
                if (strong)
                {
                    strong->sendMessage(msg);
                }
            },
            "flushAdvert");
    }
}

void
TxPullMode::shutdown()
{
    mAdvertTimer.cancel();
}

void
TxPullMode::startAdvertTimer()
{
    mAdvertTimer.expires_from_now(mApp.getConfig().FLOOD_ADVERT_PERIOD_MS);
    mAdvertTimer.async_wait([this](asio::error_code const& error) {
        if (!error)
        {
            flushAdvert();
        }
    });
}

void
TxPullMode::queueOutgoingAdvert(Hash const& txHash)
{
    if (mOutgoingTxHashes.empty())
    {
        startAdvertTimer();
    }

    mOutgoingTxHashes.emplace_back(txHash);

    // Flush adverts at the earliest of the following two conditions:
    // 1. The number of hashes reaches the threshold.
    // 2. The oldest tx hash hash been in the queue for FLOOD_TX_PERIOD_MS.
    if (mOutgoingTxHashes.size() == getMaxAdvertSize())
    {
        flushAdvert();
    }
}

bool
TxPullMode::seenAdvert(Hash const& hash)
{
    return mAdvertHistory.exists(hash);
}

void
TxPullMode::rememberHash(Hash const& hash, uint32_t ledgerSeq)
{
    mAdvertHistory.put(hash, ledgerSeq);
}

size_t
TxPullMode::size() const
{
    return mIncomingTxHashes.size() + mTxHashesToRetry.size();
}

void
TxPullMode::retryIncomingAdvert(std::list<Hash>& list)
{
    mTxHashesToRetry.splice(mTxHashesToRetry.end(), list);
    while (size() > mApp.getLedgerManager().getLastMaxTxSetSizeOps())
    {
        popIncomingAdvert();
    }
}

void
TxPullMode::queueIncomingAdvert(TxAdvertVector const& txHashes, uint32_t seq)
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
        popIncomingAdvert();
    }
}

std::pair<Hash, std::optional<VirtualClock::time_point>>
TxPullMode::popIncomingAdvert()
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
TxPullMode::clearBelow(uint32_t ledgerSeq)
{
    mAdvertHistory.erase_if(
        [&](uint32_t const& seq) { return seq < ledgerSeq; });
}

size_t
TxPullMode::getMaxAdvertSize() const
{
    auto const& cfg = mApp.getConfig();
    auto ledgerCloseTime =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            cfg.getExpectedLedgerCloseTime())
            .count();
    double opRatePerLedger = cfg.FLOOD_OP_RATE_PER_LEDGER;
    size_t maxOps = mApp.getLedgerManager().getLastMaxTxSetSizeOps();
    double opsToFloodPerLedgerDbl =
        opRatePerLedger * static_cast<double>(maxOps);
    releaseAssertOrThrow(opsToFloodPerLedgerDbl >= 0.0);
    int64_t opsToFloodPerLedger = static_cast<int64_t>(opsToFloodPerLedgerDbl);

    size_t res = static_cast<size_t>(bigDivideOrThrow(
        opsToFloodPerLedger, cfg.FLOOD_ADVERT_PERIOD_MS.count(),
        ledgerCloseTime, Rounding::ROUND_UP));

    res = std::max<size_t>(1, res);
    res = std::min<size_t>(TX_ADVERT_VECTOR_MAX_SIZE, res);
    return res;
}

}
