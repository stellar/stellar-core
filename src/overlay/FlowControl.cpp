// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/FlowControl.h"
#include "herder/Herder.h"
#include "main/Application.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "medida/timer.h"
#include "overlay/OverlayManager.h"
#include "overlay/OverlayMetrics.h"
#include "util/Logging.h"
#include <Tracy.hpp>

namespace stellar
{

constexpr std::chrono::seconds const OUTBOUND_QUEUE_TIMEOUT =
    std::chrono::seconds(30);

FlowControl::FlowControl(Application& app)
    : mApp(app)
    , mNoOutboundCapacity(
          std::make_optional<VirtualClock::time_point>(app.getClock().now()))
{
    mFlowControlCapacity =
        std::make_shared<FlowControlMessageCapacity>(mApp, mNodeID);
}

void
FlowControl::sendSendMore(uint32_t numMessages, std::shared_ptr<Peer> peer)
{
    ZoneScoped;
    StellarMessage m;
    m.type(SEND_MORE);
    m.sendMoreMessage().numMessages = numMessages;
    auto msgPtr = std::make_shared<StellarMessage const>(m);
    peer->sendMessage(msgPtr);
}

// Start flow control: send SEND_MORE to a peer to indicate available capacity
void
FlowControl::start(std::weak_ptr<Peer> peer,
                   std::function<void(StellarMessage const&)> sendCb)
{
    auto peerPtr = peer.lock();
    if (!peerPtr)
    {
        return;
    }

    mNodeID = peerPtr->getPeerID();
    mSendCallback = sendCb;
    sendSendMore(mApp.getConfig().PEER_FLOOD_READING_CAPACITY, peerPtr);
}

void
FlowControl::maybeReleaseCapacityAndTriggerSend(StellarMessage const& msg)
{
    ZoneScoped;
    if (msg.type() == SEND_MORE)
    {
        releaseAssert(getNumMessages(msg) > 0);
        mNoOutboundCapacity.reset();
        mFlowControlCapacity->releaseOutboundCapacity(msg);

        CLOG_TRACE(Overlay, "{}: Peer {} sent SEND_MORE {}",
                   mApp.getConfig().toShortString(
                       mApp.getConfig().NODE_SEED.getPublicKey()),
                   mApp.getConfig().toShortString(mNodeID),
                   getNumMessages(msg));

        // SEND_MORE means we can free some capacity, and dump the next batch of
        // messages onto the writing queue
        maybeSendNextBatch();
    }
}

void
FlowControl::maybeSendNextBatch()
{
    ZoneScoped;

    if (!mSendCallback)
    {
        throw std::runtime_error(
            "MaybeSendNextBatch: FLowControl was not started properly");
    }

    int sent = 0;
    for (int i = 0; i < mOutboundQueues.size(); i++)
    {
        auto& queue = mOutboundQueues[i];
        while (!queue.empty())
        {
            auto& front = queue.front();
            auto const& msg = *(front.mMessage);
            if (!mFlowControlCapacity->hasOutboundCapacity(msg))
            {
                break;
            }

            mSendCallback(msg);
            ++sent;
            auto& om = mApp.getOverlayManager().getOverlayMetrics();

            auto const& diff = mApp.getClock().now() - front.mTimeEmplaced;
            mFlowControlCapacity->lockOutboundCapacity(msg);
            switch (front.mMessage->type())
            {
            case TRANSACTION:
            {
                om.mOutboundQueueDelayTxs.Update(diff);
                mMetrics.mOutboundQueueDelayTxs.Update(diff);
            }
            break;
            case SCP_MESSAGE:
            {
                om.mOutboundQueueDelaySCP.Update(diff);
                mMetrics.mOutboundQueueDelaySCP.Update(diff);
            }
            break;
            case FLOOD_DEMAND:
            {
                om.mOutboundQueueDelayDemand.Update(diff);
                mMetrics.mOutboundQueueDelayDemand.Update(diff);
                size_t s = front.mMessage->floodDemand().txHashes.size();
                releaseAssert(mDemandQueueTxHashCount >= s);
                mDemandQueueTxHashCount -= s;
            }
            break;
            case FLOOD_ADVERT:
            {
                om.mOutboundQueueDelayAdvert.Update(diff);
                mMetrics.mOutboundQueueDelayAdvert.Update(diff);
                size_t s = front.mMessage->floodAdvert().txHashes.size();
                releaseAssert(mAdvertQueueTxHashCount >= s);
                mAdvertQueueTxHashCount -= s;
            }
            break;
            default:
                abort();
            }
            if (!mFlowControlCapacity->hasOutboundCapacity(msg))
            {
                CLOG_DEBUG(Overlay, "{}: No outbound capacity for peer {}",
                           mApp.getConfig().toShortString(
                               mApp.getConfig().NODE_SEED.getPublicKey()),
                           mApp.getConfig().toShortString(mNodeID));
                mNoOutboundCapacity =
                    std::make_optional<VirtualClock::time_point>(
                        mApp.getClock().now());
            }
            queue.pop_front();
        }
    }

    CLOG_TRACE(Overlay, "{} Peer {}: send next flood batch of {}",
               mApp.getConfig().toShortString(
                   mApp.getConfig().NODE_SEED.getPublicKey()),
               mApp.getConfig().toShortString(mNodeID), sent);
}

bool
FlowControl::maybeSendMessage(std::shared_ptr<StellarMessage const> msg)
{
    ZoneScoped;
    if (mApp.getOverlayManager().isFloodMessage(*msg))
    {
        addMsgAndMaybeTrimQueue(msg);
        maybeSendNextBatch();
        return true;
    }
    return false;
}

bool
FlowControl::beginMessageProcessing(StellarMessage const& msg)
{
    return mFlowControlCapacity->lockLocalCapacity(msg);
}

void
FlowControl::endMessageProcessing(StellarMessage const& msg,
                                  std::weak_ptr<Peer> peer)
{
    ZoneScoped;

    mFloodDataProcessed += mFlowControlCapacity->releaseLocalCapacity(msg);

    releaseAssert(mFloodDataProcessed <=
                  mApp.getConfig().FLOW_CONTROL_SEND_MORE_BATCH_SIZE);
    bool shouldSendMore = mFloodDataProcessed ==
                          mApp.getConfig().FLOW_CONTROL_SEND_MORE_BATCH_SIZE;
    auto peerPtr = peer.lock();

    if (shouldSendMore && peerPtr)
    {
        sendSendMore(mFloodDataProcessed, peerPtr);
        mFloodDataProcessed = 0;
    }
}

bool
FlowControl::canRead() const
{
    return mFlowControlCapacity->getCapacity().mTotalCapacity > 0;
}

uint32_t
FlowControl::getNumMessages(StellarMessage const& msg)
{
    releaseAssert(msg.type() == SEND_MORE);
    return msg.sendMoreMessage().numMessages;
}

bool
FlowControl::isSendMoreValid(StellarMessage const& msg,
                             std::string& errorMsg) const
{
    releaseAssert(msg.type() == SEND_MORE);
    if (getNumMessages(msg) == 0)
    {
        errorMsg =
            fmt::format("unexpected {} message",
                        xdr::xdr_traits<MessageType>::enum_name(msg.type()));
        return false;
    }

    auto overflow = getNumMessages(msg) >
                    (UINT64_MAX - mFlowControlCapacity->getOutboundCapacity());
    if (overflow)
    {
        errorMsg = "Peer capacity overflow";
        return false;
    }
    return true;
}

bool
dropMessageAfterTimeout(FlowControl::QueuedOutboundMessage const& queuedMsg,
                        VirtualClock::time_point now)
{
    auto const& msg = *(queuedMsg.mMessage);
    bool dropType = msg.type() == TRANSACTION || msg.type() == FLOOD_ADVERT ||
                    msg.type() == FLOOD_DEMAND;
    return dropType && (now - queuedMsg.mTimeEmplaced > OUTBOUND_QUEUE_TIMEOUT);
}

void
FlowControl::addMsgAndMaybeTrimQueue(std::shared_ptr<StellarMessage const> msg)
{
    ZoneScoped;

    releaseAssert(msg);
    auto type = msg->type();
    size_t msgQInd = 0;
    auto now = mApp.getClock().now();

    switch (type)
    {
    case SCP_MESSAGE:
    {
        msgQInd = 0;
    }
    break;
    case TRANSACTION:
    {
        msgQInd = 1;
    }
    break;
    case FLOOD_DEMAND:
    {
        msgQInd = 2;
        size_t s = msg->floodDemand().txHashes.size();
        mDemandQueueTxHashCount += s;
    }
    break;
    case FLOOD_ADVERT:
    {
        msgQInd = 3;
        size_t s = msg->floodAdvert().txHashes.size();
        mAdvertQueueTxHashCount += s;
    }
    break;
    default:
        abort();
    }
    auto& queue = mOutboundQueues[msgQInd];

    queue.emplace_back(QueuedOutboundMessage{msg, mApp.getClock().now()});

    size_t dropped = 0;

    uint32_t const limit = mApp.getLedgerManager().getLastMaxTxSetSizeOps();
    auto& om = mApp.getOverlayManager().getOverlayMetrics();
    if (type == TRANSACTION)
    {
        if (queue.size() > limit)
        {
            dropped = queue.size() - limit;
            queue.erase(queue.begin(), queue.begin() + dropped);
        }
        while (!queue.empty() && dropMessageAfterTimeout(queue.front(), now))
        {
            ++dropped;
            queue.pop_front();
        }
        om.mOutboundQueueDropTxs.Mark(dropped);
    }
    else if (type == SCP_MESSAGE)
    {
        // Iterate over the message queue. If we found any messages for slots we
        // don't keep in-memory anymore, delete those. Otherwise, compare
        // messages for the same slot and validator against the latest SCP
        // message and drop
        auto minSlotToRemember = mApp.getHerder().getMinLedgerSeqToRemember();
        bool valueReplaced = false;

        for (auto it = queue.begin(); it != queue.end();)
        {
            if (it->mMessage->envelope().statement.slotIndex <
                minSlotToRemember)
            {
                it = queue.erase(it);
                dropped++;
            }
            else if (!valueReplaced && it != queue.end() - 1 &&
                     mApp.getHerder().isNewerNominationOrBallotSt(
                         it->mMessage->envelope().statement,
                         queue.back().mMessage->envelope().statement))
            {
                valueReplaced = true;
                *it = std::move(queue.back());
                queue.pop_back();
                dropped++;
                ++it;
            }
            else
            {
                ++it;
            }
        }
        om.mOutboundQueueDropSCP.Mark(dropped);
    }
    else if (type == FLOOD_ADVERT)
    {
        while (mAdvertQueueTxHashCount > limit ||
               (!queue.empty() && dropMessageAfterTimeout(queue.front(), now)))
        {
            dropped++;
            size_t s = queue.front().mMessage->floodAdvert().txHashes.size();
            releaseAssert(mAdvertQueueTxHashCount >= s);
            mAdvertQueueTxHashCount -= s;
            queue.pop_front();
        }
        om.mOutboundQueueDropAdvert.Mark(dropped);
    }
    else if (type == FLOOD_DEMAND)
    {
        while (mDemandQueueTxHashCount > limit ||
               (!queue.empty() && dropMessageAfterTimeout(queue.front(), now)))
        {
            dropped++;
            size_t s = queue.front().mMessage->floodDemand().txHashes.size();
            releaseAssert(mDemandQueueTxHashCount >= s);
            mDemandQueueTxHashCount -= s;
            queue.pop_front();
        }
        om.mOutboundQueueDropDemand.Mark(dropped);
    }

    if (dropped && Logging::logTrace("Overlay"))
    {
        CLOG_TRACE(Overlay, "Dropped {} {} messages to peer {}", dropped,
                   xdr::xdr_traits<MessageType>::enum_name(type),
                   mApp.getConfig().toShortString(mNodeID));
    }
}

Json::Value
FlowControl::getFlowControlJsonInfo(bool compact) const
{
    Json::Value res;
    res["local_capacity"]["reading"] = static_cast<Json::UInt64>(
        mFlowControlCapacity->getCapacity().mTotalCapacity);
    res["local_capacity"]["flood"] = static_cast<Json::UInt64>(
        mFlowControlCapacity->getCapacity().mFloodCapacity);
    res["peer_capacity"] =
        static_cast<Json::UInt64>(mFlowControlCapacity->getOutboundCapacity());

    if (!compact)
    {
        res["outbound_queue_delay_scp_p75"] = static_cast<Json::UInt64>(
            mMetrics.mOutboundQueueDelaySCP.GetSnapshot().get75thPercentile());
        res["outbound_queue_delay_txs_p75"] = static_cast<Json::UInt64>(
            mMetrics.mOutboundQueueDelayTxs.GetSnapshot().get75thPercentile());
        res["outbound_queue_delay_advert_p75"] = static_cast<Json::UInt64>(
            mMetrics.mOutboundQueueDelayAdvert.GetSnapshot()
                .get75thPercentile());
        res["outbound_queue_delay_demand_p75"] = static_cast<Json::UInt64>(
            mMetrics.mOutboundQueueDelayDemand.GetSnapshot()
                .get75thPercentile());
    }

    return res;
}

FlowControl::FlowControlMetrics::FlowControlMetrics()
    : mOutboundQueueDelaySCP(medida::Timer(Peer::PEER_METRICS_DURATION_UNIT,
                                           Peer::PEER_METRICS_RATE_UNIT,
                                           Peer::PEER_METRICS_WINDOW_SIZE))
    , mOutboundQueueDelayTxs(medida::Timer(Peer::PEER_METRICS_DURATION_UNIT,
                                           Peer::PEER_METRICS_RATE_UNIT,
                                           Peer::PEER_METRICS_WINDOW_SIZE))
    , mOutboundQueueDelayAdvert(medida::Timer(Peer::PEER_METRICS_DURATION_UNIT,
                                              Peer::PEER_METRICS_RATE_UNIT,
                                              Peer::PEER_METRICS_WINDOW_SIZE))
    , mOutboundQueueDelayDemand(medida::Timer(Peer::PEER_METRICS_DURATION_UNIT,
                                              Peer::PEER_METRICS_RATE_UNIT,
                                              Peer::PEER_METRICS_WINDOW_SIZE))
{
}
}