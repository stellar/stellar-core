// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/TxAdverts.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "util/ProtocolVersion.h"
#include <algorithm>

namespace stellar
{

constexpr uint32 const ADVERT_CACHE_SIZE = 50000;

TxAdverts::TxAdverts(Application& app)
    : mApp(app), mAdvertHistory(ADVERT_CACHE_SIZE), mAdvertTimer(app)
{
}

void
TxAdverts::flushAdvert()
{
    if (mOutgoingTxHashes.size() > 0)
    {
        auto msg = std::make_shared<StellarMessage>();
        msg->type(FLOOD_ADVERT);
        msg->floodAdvert().txHashes = std::move(mOutgoingTxHashes);

        mOutgoingTxHashes.clear();
        mApp.postOnMainThread(
            [send = mSendCb, msg = std::move(msg)]() {
                releaseAssert(send);
                send(msg);
            },
            "flushAdvert");
    }
}

void
TxAdverts::shutdown()
{
    mAdvertTimer.cancel();
}

void
TxAdverts::start(
    std::function<void(std::shared_ptr<StellarMessage const>)> sendCb)
{
    if (!sendCb)
    {
        throw std::invalid_argument("sendCb must be set");
    }
    mSendCb = sendCb;
}

void
TxAdverts::startAdvertTimer()
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
TxAdverts::queueOutgoingAdvert(Hash const& txHash)
{
    if (mOutgoingTxHashes.empty())
    {
        startAdvertTimer();
    }

    mOutgoingTxHashes.emplace_back(txHash);

    // Flush adverts at the earliest of the following two conditions:
    // 1. The number of hashes reaches the threshold (see condition below).
    // 2. The oldest tx hash hash been in the queue for FLOOD_TX_PERIOD_MS
    // (managed via mAdvertTimer).
    if (mOutgoingTxHashes.size() == getMaxAdvertSize())
    {
        flushAdvert();
    }
}

bool
TxAdverts::seenAdvert(Hash const& hash)
{
    return mAdvertHistory.exists(hash);
}

void
TxAdverts::rememberHash(Hash const& hash, uint32_t ledgerSeq)
{
    mAdvertHistory.put(hash, ledgerSeq);
}

size_t
TxAdverts::size() const
{
    return mIncomingTxHashes.size() + mTxHashesToRetry.size();
}

void
TxAdverts::retryIncomingAdvert(std::list<Hash>& list)
{
    mTxHashesToRetry.splice(mTxHashesToRetry.end(), list);
    while (size() > mApp.getLedgerManager().getLastMaxTxSetSizeOps())
    {
        popIncomingAdvert();
    }
}

void
TxAdverts::queueIncomingAdvert(TxAdvertVector const& txHashes, uint32_t seq)
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
TxAdverts::popIncomingAdvert()
{
    if (size() <= 0)
    {
        throw std::runtime_error("No advert to pop");
    }

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
TxAdverts::clearBelow(uint32_t ledgerSeq)
{
    mAdvertHistory.erase_if(
        [&](uint32_t const& seq) { return seq < ledgerSeq; });
}

int64_t
TxAdverts::getOpsFloodLedger(size_t maxOps, double rate)
{
    double opsToFloodPerLedgerDbl = rate * static_cast<double>(maxOps);
    releaseAssertOrThrow(opsToFloodPerLedgerDbl >= 0.0);
    return static_cast<int64_t>(opsToFloodPerLedgerDbl);
}

size_t
TxAdverts::getMaxAdvertSize() const
{
    auto const& cfg = mApp.getConfig();
    auto ledgerCloseTime =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            cfg.getExpectedLedgerCloseTime())
            .count();

    int64_t opsToFloodPerLedger =
        getOpsFloodLedger(mApp.getLedgerManager().getLastMaxTxSetSizeOps(),
                          cfg.FLOOD_OP_RATE_PER_LEDGER);

    {
        if (protocolVersionStartsFrom(mApp.getLedgerManager()
                                          .getLastClosedLedgerHeader()
                                          .header.ledgerVersion,
                                      SOROBAN_PROTOCOL_VERSION))
        {
            auto limits =
                mApp.getLedgerManager().getSorobanNetworkConfigReadOnly();
            opsToFloodPerLedger += getOpsFloodLedger(
                limits.ledgerMaxTxCount(), cfg.FLOOD_SOROBAN_RATE_PER_LEDGER);
        }
    }

    size_t res = static_cast<size_t>(bigDivideOrThrow(
        opsToFloodPerLedger, cfg.FLOOD_ADVERT_PERIOD_MS.count(),
        ledgerCloseTime, Rounding::ROUND_UP));

    return std::clamp<size_t>(res, 1, TX_ADVERT_VECTOR_MAX_SIZE);
}

}
