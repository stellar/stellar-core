// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/TxDemandsManager.h"
#include "crypto/Hex.h"
#include "herder/Herder.h"
#include "medida/meter.h"
#include "overlay/FlowControlCapacity.h"
#include "overlay/OverlayManager.h"
#include "overlay/OverlayMetrics.h"
#include "overlay/TxAdverts.h"
#include "util/Logging.h"
#include "util/numeric.h"
#include <Tracy.hpp>
#include <algorithm>

namespace stellar
{

// Regardless of the number of failed attempts &
// FLOOD_DEMAND_BACKOFF_DELAY_MS it doesn't make much sense to wait much
// longer than 2 seconds between re-issuing demands.
constexpr std::chrono::seconds MAX_DELAY_DEMAND{2};

TxDemandsManager::TxDemandsManager(Application& app)
    : mApp(app), mDemandTimer(app)
{
}

void
TxDemandsManager::start()
{
    demand();
}

void
TxDemandsManager::shutdown()
{
    mDemandTimer.cancel();
}

size_t
TxDemandsManager::getMaxDemandSize() const
{
    auto const& cfg = mApp.getConfig();
    auto ledgerCloseTime =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            cfg.getExpectedLedgerCloseTime())
            .count();
    int64_t queueSizeInOps = TxAdverts::getOpsFloodLedger(
        mApp.getHerder().getMaxQueueSizeOps(), cfg.FLOOD_OP_RATE_PER_LEDGER);

    queueSizeInOps += TxAdverts::getOpsFloodLedger(
        mApp.getHerder().getMaxQueueSizeSorobanOps(),
        cfg.FLOOD_SOROBAN_RATE_PER_LEDGER);

    size_t res = static_cast<size_t>(
        bigDivideOrThrow(queueSizeInOps, cfg.FLOOD_DEMAND_PERIOD_MS.count(),
                         ledgerCloseTime, Rounding::ROUND_UP));
    return std::clamp<size_t>(res, 1, TX_DEMAND_VECTOR_MAX_SIZE);
}

std::chrono::milliseconds
TxDemandsManager::retryDelayDemand(int numAttemptsMade) const
{
    auto res = numAttemptsMade * mApp.getConfig().FLOOD_DEMAND_BACKOFF_DELAY_MS;
    return std::min(res, std::chrono::milliseconds(MAX_DELAY_DEMAND));
}

TxDemandsManager::DemandStatus
TxDemandsManager::demandStatus(Hash const& txHash, Peer::pointer peer) const
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
TxDemandsManager::startDemandTimer()
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
TxDemandsManager::demand()
{
    ZoneScoped;
    if (mApp.getOverlayManager().isShuttingDown())
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

    // We randomize peers here to avoid biasing demand pressure to any one
    // particular peer
    auto peers = mApp.getOverlayManager().getRandomAuthenticatedPeers();

    UnorderedMap<Peer::pointer, std::pair<TxDemandVector, std::list<Hash>>>
        demandMap;
    bool anyNewDemand = false;
    auto maxDemandSize = getMaxDemandSize();
    do
    {
        anyNewDemand = false;
        for (auto const& peer : peers)
        {
            auto& demPair = demandMap[peer];
            auto& demand = demPair.first;
            auto& retry = demPair.second;
            bool addedNewDemand = false;

            while (demand.size() < maxDemandSize && peer->hasAdvert() &&
                   !addedNewDemand)
            {
                auto txHash = peer->popAdvert();
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
        // Loop again if we added one new demand to any peer
    } while (anyNewDemand);

    for (auto const& peer : peers)
    {
        // We move `demand` here and also pass `retry` as a reference
        // which gets appended. Don't touch `demand` or `retry` after here.
        peer->sendTxDemand(std::move(demandMap[peer].first));
        peer->retryAdvert(demandMap[peer].second);
    }

    // mPendingDemands and mDemandHistoryMap must always contain exactly the
    // same tx hashes, compare sizes as a sanity check
    releaseAssert(mPendingDemands.size() == mDemandHistoryMap.size());
    startDemandTimer();
}

void
TxDemandsManager::recordTxPullLatency(Hash const& hash,
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

void
TxDemandsManager::recvTxDemand(FloodDemand const& dmd, Peer::pointer peer)
{
    ZoneScoped;
    auto& herder = mApp.getHerder();
    auto& om = mApp.getOverlayManager().getOverlayMetrics();
#ifdef BUILD_TESTS
    auto msg = std::make_shared<StellarMessage>();
    size_t batchSize = 0;
    if (mApp.getConfig().EXPERIMENTAL_TX_BATCH_MAX_SIZE > 0)
    {
        msg = OverlayManager::createTxBatch();
    }

    auto sendAndReset = [&]() {
        if (batchSize > 0)
        {
            om.mTxBatchSizeHistogram.Update(batchSize);
            peer->sendMessage(std::move(msg));
            msg = OverlayManager::createTxBatch();
            batchSize = 0;
        }
    };
#endif

    for (auto const& h : dmd.txHashes)
    {
        auto tx = herder.getTx(h);
        if (tx)
        {
            // The tx exists
            CLOG_TRACE(Overlay, "fulfilled demand for {} demanded by {}",
                       hexAbbrev(h),
                       KeyUtils::toShortString(peer->getPeerID()));
            peer->getPeerMetrics().mMessagesFulfilled++;
            om.mMessagesFulfilledMeter.Mark();

#ifdef BUILD_TESTS
            if (mApp.getConfig().EXPERIMENTAL_TX_BATCH_MAX_SIZE > 0)
            {
                // Current batch size
                auto currSize = FlowControlCapacity::msgBodySize(*msg);
                // New tx size to append
                auto newTxSize =
                    FlowControlCapacity::msgBodySize(*tx->toStellarMessage());

                // Send existing batch if it exceeds the max size, create a new
                // message
                auto maxSize = mApp.getHerder().getMaxTxSize();
                if ((currSize + newTxSize) > maxSize)
                {
                    if (currSize > maxSize || newTxSize > maxSize)
                    {
                        throw std::runtime_error(fmt::format(
                            "Transaction size {} exceeds maximum allowed size "
                            "{}",
                            newTxSize, maxSize));
                    }
                    sendAndReset();
                }

                msg->txSet().txs.emplace_back(
                    tx->toStellarMessage()->transaction());
                batchSize++;

                if (msg->txSet().txs.size() ==
                    mApp.getConfig().EXPERIMENTAL_TX_BATCH_MAX_SIZE)
                {
                    sendAndReset();
                }
            }
            else
#endif
            {
                peer->sendMessage(tx->toStellarMessage());
            }
        }
        else
        {
            auto banned = herder.isBannedTx(h);
            CLOG_TRACE(Overlay,
                       "can't fulfill demand for {} hash {} demanded by {}",
                       banned ? "banned" : "unknown", hexAbbrev(h),
                       KeyUtils::toShortString(peer->getPeerID()));
            if (banned)
            {
                om.mBannedMessageUnfulfilledMeter.Mark();
                peer->getPeerMetrics().mBannedMessageUnfulfilled++;
            }
            else
            {
                om.mUnknownMessageUnfulfilledMeter.Mark();
                peer->getPeerMetrics().mUnknownMessageUnfulfilled++;
            }
        }
    }

#ifdef BUILD_TESTS
    // Send any remaining transactions in the batch and record the size
    if (mApp.getConfig().EXPERIMENTAL_TX_BATCH_MAX_SIZE > 0 &&
        !msg->txSet().txs.empty())
    {
        sendAndReset();
    }
#endif
}
}