// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/TxFloodManager.h"
#include "crypto/Hex.h"
#include "herder/Herder.h"
#include "ledger/LedgerManager.h"
#include "medida/meter.h"
#include "overlay/OverlayManager.h"
#include "overlay/OverlayMetrics.h"
#include "util/Logging.h"
#include "util/numeric.h"
#include <Tracy.hpp>

namespace stellar
{

// Regardless of the number of failed attempts &
// FLOOD_DEMAND_BACKOFF_DELAY_MS it doesn't make much sense to wait much
// longer than 2 seconds between re-issuing demands.
constexpr std::chrono::seconds MAX_DELAY_DEMAND{2};

TxFloodManager::TxFloodManager(Application& app)
    : mApp(app), mAdvertTimer(app), mDemandTimer(app)
{
}

bool
TxFloodManager::peerKnowsHash(Hash const& txHash, Peer::pointer peer)
{
    ZoneScoped;
    auto it = mQueuedIncomingAdverts.find(peer);
    if (it != mQueuedIncomingAdverts.end())
    {
        return it->second->peerKnowsHash(txHash);
    }
    return false;
}

std::unique_ptr<TxFloodManager>
TxFloodManager::create(Application& app)
{
    return std::make_unique<TxFloodManager>(app);
}

void
TxFloodManager::flushAdvert(Peer::pointer peer)
{
    auto& outgoingAdverts = mQueuedOutgoingAdverts[peer];
    if (outgoingAdverts.size() > 0)
    {
        StellarMessage adv;
        adv.type(FLOOD_ADVERT);

        adv.floodAdvert().txHashes = std::move(outgoingAdverts);
        auto msg = std::make_shared<StellarMessage>(adv);
        std::weak_ptr<Peer> weak(peer);
        mQueuedOutgoingAdverts.erase(peer);
        mApp.postOnMainThread(
            [weak, msg = std::move(msg)]() {
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
TxFloodManager::startAdvertTimer()
{
    if (mShuttingDown)
    {
        return;
    }
    mAdvertTimer.expires_from_now(mApp.getConfig().FLOOD_ADVERT_PERIOD_MS);
    mAdvertTimer.async_wait([this](asio::error_code const& error) {
        if (!error)
        {
            auto peers = mApp.getOverlayManager().getRandomAuthenticatedPeers();
            for (auto& peer : peers)
            {
                flushAdvert(peer);
            }
        }
    });
}

void
TxFloodManager::queueOutgoingTxHash(Hash const& txHash, Peer::pointer peer)
{
    auto const empty = mQueuedIncomingAdverts.empty();
    auto& outgoingAdverts = mQueuedOutgoingAdverts[peer];

    if (outgoingAdverts.size() == TX_ADVERT_VECTOR_MAX_SIZE)
    {
        CLOG_TRACE(Overlay, "{}'s tx hash queue is full, dropping {}",
                   peer->toString(), hexAbbrev(txHash));
        return;
    }

    outgoingAdverts.emplace_back(txHash);

    // Flush adverts at the earliest of the following two conditions:
    // 1. The number of hashes reaches the threshold.
    // 2. The oldest tx hash hash been in the queue for FLOOD_TX_PERIOD_MS.
    if (outgoingAdverts.size() == getMaxAdvertSize())
    {
        flushAdvert(peer);
    }
    if (empty)
    {
        startAdvertTimer();
    }
}

void
TxFloodManager::queueIncomingTxAdvert(TxAdvertVector const& advert,
                                      uint32_t ledgerSeq, Peer::pointer peer)
{
    bool emptyQueue = mQueuedIncomingAdverts.empty();
    auto it = mQueuedIncomingAdverts.find(peer);
    if (it == mQueuedIncomingAdverts.end())
    {
        mQueuedIncomingAdverts.emplace(
            std::make_pair(peer, std::make_unique<TxAdvertQueue>(mApp)));
    }
    it->second->queueAndMaybeTrim(advert, ledgerSeq);
    if (emptyQueue)
    {
        startDemandTimer();
    }
}

void
TxFloodManager::shutdown()
{
    if (mShuttingDown)
    {
        return;
    }
    mShuttingDown = true;
    mAdvertTimer.cancel();
}

size_t
TxFloodManager::getMaxDemandSize() const
{
    auto const& cfg = mApp.getConfig();
    auto ledgerCloseTime =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            cfg.getExpectedLedgerCloseTime())
            .count();
    double opRatePerLedger = cfg.FLOOD_OP_RATE_PER_LEDGER;
    double queueSizeInOpsDbl =
        static_cast<double>(mApp.getHerder().getMaxQueueSizeOps()) *
        opRatePerLedger;
    releaseAssertOrThrow(queueSizeInOpsDbl >= 0.0);
    int64_t queueSizeInOps = static_cast<int64_t>(queueSizeInOpsDbl);

    size_t res = static_cast<size_t>(
        bigDivideOrThrow(queueSizeInOps, cfg.FLOOD_DEMAND_PERIOD_MS.count(),
                         ledgerCloseTime, Rounding::ROUND_UP));
    res = std::max<size_t>(1, res);
    res = std::min<size_t>(TX_DEMAND_VECTOR_MAX_SIZE, res);
    return res;
}

std::chrono::milliseconds
TxFloodManager::retryDelayDemand(int numAttemptsMade) const
{
    auto res = numAttemptsMade * mApp.getConfig().FLOOD_DEMAND_BACKOFF_DELAY_MS;
    return std::min(res, std::chrono::milliseconds(MAX_DELAY_DEMAND));
}

TxFloodManager::DemandStatus
TxFloodManager::demandStatus(Hash const& txHash, Peer::pointer peer) const
{
    if (mApp.getHerder().isBannedTx(txHash) ||
        mApp.getHerder().getTx(txHash) != nullptr)
    {
        return DemandStatus::DISCARD;
    }
    auto it = mDemandHistoryMap.find(txHash);
    if (it == mDemandHistoryMap.end())
    {
        // never demanded
        return DemandStatus::DEMAND;
    }
    auto& demandedPeers = it->second.peers;
    if (demandedPeers.find(peer->getPeerID()) != demandedPeers.end())
    {
        // We've already demanded.
        return DemandStatus::DISCARD;
    }
    int const numDemanded = static_cast<int>(demandedPeers.size());
    auto const lastDemanded = it->second.lastDemanded;

    if (numDemanded < MAX_RETRY_COUNT)
    {
        // Check if it's been a while since our last demand
        if ((mApp.getClock().now() - lastDemanded) >=
            retryDelayDemand(numDemanded))
        {
            return DemandStatus::DEMAND;
        }
        else
        {
            return DemandStatus::RETRY_LATER;
        }
    }
    return DemandStatus::DISCARD;
}

void
TxFloodManager::startDemandTimer()
{
    mDemandTimer.expires_from_now(mApp.getConfig().FLOOD_DEMAND_PERIOD_MS);
    mDemandTimer.async_wait([this](asio::error_code const& error) {
        if (!error)
        {
            this->demand();
        }
    });
}

void
TxFloodManager::demand()
{
    ZoneScoped;
    if (mShuttingDown)
    {
        return;
    }
    auto const now = mApp.getClock().now();

    auto& om = mApp.getOverlayManager().getOverlayMetrics();

    // We determine that demands are obsolete after maxRetention.
    auto maxRetention = MAX_DELAY_DEMAND * MAX_RETRY_COUNT * 2;
    while (!mPendingDemands.empty())
    {
        auto const& it = mDemandHistoryMap.find(mPendingDemands.front());
        if ((now - it->second.firstDemanded) >= maxRetention)
        {
            if (!it->second.latencyRecorded)
            {
                // We never received the txn.
                om.mAbandonedDemandMeter.Mark();
            }
            mPendingDemands.pop();
            mDemandHistoryMap.erase(it);
        }
        else
        {
            // The oldest demand in mPendingDemands isn't old enough
            // to be deleted from our record.
            break;
        }
    }

    auto peers = mApp.getOverlayManager().getRandomAuthenticatedPeers();

    UnorderedMap<Peer::pointer, std::pair<TxDemandVector, std::list<Hash>>>
        demandMap;
    bool anyNewDemand = false;
    do
    {
        anyNewDemand = false;
        for (auto const& peer : peers)
        {
            auto& demPair = demandMap[peer];
            auto& demand = demPair.first;
            auto& retry = demPair.second;
            bool addedNewDemand = false;

            auto it = mQueuedIncomingAdverts.find(peer);
            if (it == mQueuedIncomingAdverts.end())
            {
                continue;
            }
            auto& queue = *(it->second);
            while (demand.size() < getMaxDemandSize() && queue.size() > 0 &&
                   !addedNewDemand)
            {
                auto hashPair = queue.pop();
                auto txHash = hashPair.first;
                if (hashPair.second)
                {
                    auto delta = now - *(hashPair.second);
                    om.mAdvertQueueDelay.Update(delta);
                    peer->getPeerMetrics().mAdvertQueueDelay.Update(delta);
                }
                switch (demandStatus(txHash, peer))
                {
                case DemandStatus::DEMAND:
                    demand.push_back(txHash);
                    if (mDemandHistoryMap.find(txHash) ==
                        mDemandHistoryMap.end())
                    {
                        // We don't have any pending demand record of this tx
                        // hash.
                        mPendingDemands.push(txHash);
                        mDemandHistoryMap[txHash].firstDemanded = now;
                        CLOG_DEBUG(Overlay, "Demand tx {}, asking peer {}",
                                   hexAbbrev(txHash), peer->toString());
                    }
                    else
                    {
                        om.mDemandTimeouts.Mark();
                        ++(peer->getPeerMetrics().mDemandTimeouts);
                    }
                    mDemandHistoryMap[txHash].peers.emplace(peer->getPeerID(),
                                                            now);
                    mDemandHistoryMap[txHash].lastDemanded = now;
                    addedNewDemand = true;
                    break;
                case DemandStatus::RETRY_LATER:
                    retry.push_back(txHash);
                    break;
                case DemandStatus::DISCARD:
                    break;
                }
            }
            anyNewDemand |= addedNewDemand;
        }
    } while (anyNewDemand);

    for (auto const& peer : peers)
    {
        // We move `demand` here and also pass `retry` as a reference
        // which gets appended. Don't touch `demand` or `retry` after here.
        peer->sendTxDemand(std::move(demandMap[peer].first));
        if (demandMap[peer].second.empty())
        {
            continue;
        }
        auto it = mQueuedIncomingAdverts.find(peer);
        if (it == mQueuedIncomingAdverts.end())
        {
            mQueuedIncomingAdverts.emplace(
                peer, std::make_unique<TxAdvertQueue>(mApp));
        }
        it->second->appendHashesToRetryAndMaybeTrim(demandMap[peer].second);
    }

    // mPendingDemands and mDemandHistoryMap must always contain exactly the
    // same tx hashes.
    releaseAssert(mPendingDemands.size() == mDemandHistoryMap.size());
    startDemandTimer();
}

void
TxFloodManager::recordTxPullLatency(Hash const& hash,
                                    std::shared_ptr<Peer> peer)
{
    auto it = mDemandHistoryMap.find(hash);
    auto now = mApp.getClock().now();
    auto& om = mApp.getOverlayManager().getOverlayMetrics();
    if (it != mDemandHistoryMap.end())
    {
        // Record end-to-end pull time
        if (!it->second.latencyRecorded)
        {
            auto delta = now - it->second.firstDemanded;
            om.mTxPullLatency.Update(delta);
            it->second.latencyRecorded = true;
            CLOG_DEBUG(
                Overlay,
                "Pulled transaction {} in {} milliseconds, asked {} peers",
                hexAbbrev(hash),
                std::chrono::duration_cast<std::chrono::milliseconds>(delta)
                    .count(),
                it->second.peers.size());
        }

        // Record pull time from individual peer
        auto peerIt = it->second.peers.find(peer->getPeerID());
        if (peerIt != it->second.peers.end())
        {
            auto delta = now - peerIt->second;
            om.mPeerTxPullLatency.Update(delta);
            peer->getPeerMetrics().mPullLatency.Update(delta);
            CLOG_DEBUG(
                Overlay,
                "Pulled transaction {} in {} milliseconds from peer {}",
                hexAbbrev(hash),
                std::chrono::duration_cast<std::chrono::milliseconds>(delta)
                    .count(),
                peer->toString());
        }
    }
}

size_t
TxFloodManager::getMaxAdvertSize() const
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

void
TxFloodManager::clearBelow(uint32_t ledgerSeq)
{
    for (auto& pair : mQueuedIncomingAdverts)
    {
        pair.second->clearBelow(ledgerSeq);
    }
}

}