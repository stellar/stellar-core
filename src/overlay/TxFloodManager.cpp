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

TxFloodManager::TxFloodManager(Application& app) : mApp(app), mDemandTimer(app)
{
}

std::unique_ptr<TxFloodManager>
TxFloodManager::create(Application& app)
{
    return std::make_unique<TxFloodManager>(app);
}

void
TxFloodManager::start()
{
    demand();
}

void
TxFloodManager::shutdown()
{
    mDemandTimer.cancel();
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

            while (demand.size() < getMaxDemandSize() && peer->hasAdvert() &&
                   !addedNewDemand)
            {
                auto hashPair = peer->popAdvert();
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
        peer->retryAdvert(demandMap[peer].second);
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

}