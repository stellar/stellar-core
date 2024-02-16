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

size_t
FlowControl::getOutboundQueueByteLimit() const
{
#ifdef BUILD_TESTS
    if (mOutboundQueueLimit)
    {
        return *mOutboundQueueLimit;
    }
#endif
    return mApp.getConfig().OUTBOUND_TX_QUEUE_BYTE_LIMIT;
}

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

void
FlowControl::sendSendMore(uint32_t numMessages, uint32_t numBytes,
                          std::shared_ptr<Peer> peer)
{
    ZoneScoped;
    StellarMessage m;
    m.type(SEND_MORE_EXTENDED);
    m.sendMoreExtendedMessage().numMessages = numMessages;
    m.sendMoreExtendedMessage().numBytes = numBytes;

    auto msgPtr = std::make_shared<StellarMessage const>(m);
    peer->sendMessage(msgPtr);
}

bool
FlowControl::hasOutboundCapacity(StellarMessage const& msg) const
{
    releaseAssert(mFlowControlCapacity);
    return mFlowControlCapacity->hasOutboundCapacity(msg) &&
           (!mFlowControlBytesCapacity ||
            mFlowControlBytesCapacity->hasOutboundCapacity(msg));
}

// Start flow control: send SEND_MORE to a peer to indicate available capacity
void
FlowControl::start(std::weak_ptr<Peer> peer,
                   std::function<void(StellarMessage const&)> sendCb,
                   bool enableFCBytes)
{
    auto peerPtr = peer.lock();
    if (!peerPtr)
    {
        return;
    }

    mNodeID = peerPtr->getPeerID();
    mSendCallback = sendCb;

    if (enableFCBytes)
    {
        mFlowControlBytesCapacity =
            std::make_shared<FlowControlByteCapacity>(mApp, mNodeID);
        sendSendMore(
            mApp.getConfig().PEER_FLOOD_READING_CAPACITY,
            mApp.getOverlayManager().getFlowControlBytesConfig().mTotal,
            peerPtr);
    }
    else
    {
        sendSendMore(mApp.getConfig().PEER_FLOOD_READING_CAPACITY, peerPtr);
    }
}

void
FlowControl::maybeReleaseCapacityAndTriggerSend(StellarMessage const& msg)
{
    ZoneScoped;

    if (msg.type() == SEND_MORE || msg.type() == SEND_MORE_EXTENDED)
    {
        if (mNoOutboundCapacity)
        {
            mApp.getOverlayManager()
                .getOverlayMetrics()
                .mConnectionFloodThrottle.Update(mApp.getClock().now() -
                                                 *mNoOutboundCapacity);
        }
        mNoOutboundCapacity.reset();

        mFlowControlCapacity->releaseOutboundCapacity(msg);
        if (mFlowControlBytesCapacity)
        {
            mFlowControlBytesCapacity->releaseOutboundCapacity(msg);
        }

        CLOG_TRACE(Overlay, "{}: Peer {} sent {} ({} messages, {} bytes)",
                   mApp.getConfig().toShortString(
                       mApp.getConfig().NODE_SEED.getPublicKey()),
                   mApp.getConfig().toShortString(mNodeID),
                   xdr::xdr_traits<MessageType>::enum_name(msg.type()),
                   getNumMessages(msg),
                   mFlowControlBytesCapacity
                       ? std::to_string(msg.sendMoreExtendedMessage().numBytes)
                       : "N/A");

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
            // Can't send _current_ message
            if (!hasOutboundCapacity(msg))
            {
                CLOG_DEBUG(Overlay, "{}: No outbound capacity for peer {}",
                           mApp.getConfig().toShortString(
                               mApp.getConfig().NODE_SEED.getPublicKey()),
                           mApp.getConfig().toShortString(mNodeID));
                // Start a timeout for SEND_MORE
                mNoOutboundCapacity =
                    std::make_optional<VirtualClock::time_point>(
                        mApp.getClock().now());
                break;
            }

            mSendCallback(msg);
            ++sent;
            auto& om = mApp.getOverlayManager().getOverlayMetrics();

            auto const& diff = mApp.getClock().now() - front.mTimeEmplaced;
            mFlowControlCapacity->lockOutboundCapacity(msg);
            if (mFlowControlBytesCapacity)
            {
                mFlowControlBytesCapacity->lockOutboundCapacity(msg);
            }

            switch (front.mMessage->type())
            {
            case TRANSACTION:
            {
                om.mOutboundQueueDelayTxs.Update(diff);
                mMetrics.mOutboundQueueDelayTxs.Update(diff);
                if (mFlowControlBytesCapacity)
                {
                    size_t s =
                        mFlowControlBytesCapacity->getMsgResourceCount(msg);
                    releaseAssert(mTxQueueByteCount >= s);
                    mTxQueueByteCount -= s;
                }
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

void
FlowControl::handleTxSizeIncrease(uint32_t increase, std::shared_ptr<Peer> peer)
{
    ZoneScoped;
    if (mFlowControlBytesCapacity)
    {
        releaseAssert(increase > 0);
        // Bump flood capacity to accommodate the upgrade
        mFlowControlBytesCapacity->handleTxSizeIncrease(increase);
        // Send an additional SEND_MORE to let the other peer know we have more
        // capacity available (and possibly unblock it)
        sendSendMore(0, increase, peer);
    }
}

bool
FlowControl::beginMessageProcessing(StellarMessage const& msg)
{
    ZoneScoped;

    return mFlowControlCapacity->lockLocalCapacity(msg) &&
           (!mFlowControlBytesCapacity ||
            mFlowControlBytesCapacity->lockLocalCapacity(msg));
}

void
FlowControl::endMessageProcessing(StellarMessage const& msg,
                                  std::weak_ptr<Peer> peer)
{
    ZoneScoped;

    mFloodDataProcessed += mFlowControlCapacity->releaseLocalCapacity(msg);
    if (mFlowControlBytesCapacity)
    {
        mFloodDataProcessedBytes +=
            mFlowControlBytesCapacity->releaseLocalCapacity(msg);
    }

    releaseAssert(mFloodDataProcessed <=
                  mApp.getConfig().FLOW_CONTROL_SEND_MORE_BATCH_SIZE);
    bool shouldSendMore = mFloodDataProcessed ==
                          mApp.getConfig().FLOW_CONTROL_SEND_MORE_BATCH_SIZE;
    if (mFlowControlBytesCapacity)
    {
        auto const byteBatchSize =
            mApp.getOverlayManager().getFlowControlBytesConfig().mBatchSize;
        shouldSendMore =
            shouldSendMore || mFloodDataProcessedBytes >= byteBatchSize;
    }
    auto peerPtr = peer.lock();

    if (shouldSendMore && peerPtr)
    {
        if (mFlowControlBytesCapacity)
        {
            sendSendMore(static_cast<uint32>(mFloodDataProcessed),
                         static_cast<uint32>(mFloodDataProcessedBytes),
                         peerPtr);
        }
        else
        {
            sendSendMore(static_cast<uint32>(mFloodDataProcessed), peerPtr);
            releaseAssert(mFloodDataProcessedBytes == 0);
        }
        mFloodDataProcessed = 0;
        mFloodDataProcessedBytes = 0;
    }
}

bool
FlowControl::canRead() const
{
    bool canReadBytes =
        !mFlowControlBytesCapacity || mFlowControlBytesCapacity->canRead();
    return canReadBytes && mFlowControlCapacity->canRead();
}

uint32_t
FlowControl::getNumMessages(StellarMessage const& msg)
{
    return msg.type() == SEND_MORE ? msg.sendMoreMessage().numMessages
                                   : msg.sendMoreExtendedMessage().numMessages;
}

bool
FlowControl::isSendMoreValid(StellarMessage const& msg,
                             std::string& errorMsg) const
{
    bool sendMoreExtendedType =
        mFlowControlBytesCapacity && msg.type() == SEND_MORE_EXTENDED;
    bool sendMoreType = !mFlowControlBytesCapacity && msg.type() == SEND_MORE;

    if (!sendMoreExtendedType && !sendMoreType)
    {
        errorMsg =
            fmt::format("unexpected message type {}",
                        xdr::xdr_traits<MessageType>::enum_name(msg.type()));
        return false;
    }

    // If flow control in bytes isn't enabled, SEND_MORE must have non-zero
    // messages. If flow control in bytes is enabled, SEND_MORE_EXTENDED must
    // have non-zero bytes, but _can_ have 0 messages to support upgrades
    if ((!mFlowControlBytesCapacity && getNumMessages(msg) == 0) ||
        (mFlowControlBytesCapacity &&
         msg.sendMoreExtendedMessage().numBytes == 0))
    {
        errorMsg =
            fmt::format("invalid message {}",
                        xdr::xdr_traits<MessageType>::enum_name(msg.type()));
        return false;
    }

    auto overflow = getNumMessages(msg) >
                    (UINT64_MAX - mFlowControlCapacity->getOutboundCapacity());
    if (mFlowControlBytesCapacity)
    {
        overflow =
            overflow ||
            (msg.sendMoreExtendedMessage().numBytes >
             (UINT64_MAX - mFlowControlBytesCapacity->getOutboundCapacity()));
    }
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
        if (mFlowControlBytesCapacity)
        {
            auto bytes = mFlowControlBytesCapacity->getMsgResourceCount(*msg);
            // Don't accept transactions that are over allowed byte limit: those
            // won't be properly flooded anyways
            if (bytes > mApp.getHerder().getMaxTxSize())
            {
                return;
            }
            mTxQueueByteCount += bytes;
        }
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
        auto isOverLimit = [&](auto const& queue) {
            bool overLimit = queue.size() > limit;
            if (mFlowControlBytesCapacity)
            {
                overLimit = overLimit ||
                            mTxQueueByteCount > getOutboundQueueByteLimit();
            }
            // Time-based purge
            overLimit =
                overLimit ||
                (!queue.empty() && dropMessageAfterTimeout(queue.front(), now));
            return overLimit;
        };

        // Message/byte limit purge
        while (isOverLimit(queue))
        {
            dropped++;
            if (mFlowControlBytesCapacity)
            {
                size_t s = mFlowControlBytesCapacity->getMsgResourceCount(
                    *(queue.front().mMessage));
                releaseAssert(mTxQueueByteCount >= s);
                mTxQueueByteCount -= s;
            }
            om.mOutboundQueueDropTxs.Mark(dropped);
            queue.pop_front();
        }
    }
    else if (type == SCP_MESSAGE)
    {
        // Iterate over the message queue. If we found any messages for slots we
        // don't keep in-memory anymore, delete those. Otherwise, compare
        // messages for the same slot and validator against the latest SCP
        // message and drop
        auto minSlotToRemember = mApp.getHerder().getMinLedgerSeqToRemember();
        auto checkpointSeq = mApp.getHerder().getMostRecentCheckpointSeq();
        bool valueReplaced = false;

        for (auto it = queue.begin(); it != queue.end();)
        {
            if (auto index = it->mMessage->envelope().statement.slotIndex;
                index < minSlotToRemember && index != checkpointSeq)
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
    if (mFlowControlCapacity->getCapacity().mTotalCapacity)
    {
        res["local_capacity"]["reading"] = static_cast<Json::UInt64>(
            *(mFlowControlCapacity->getCapacity().mTotalCapacity));
    }
    res["local_capacity"]["flood"] = static_cast<Json::UInt64>(
        mFlowControlCapacity->getCapacity().mFloodCapacity);
    res["peer_capacity"] =
        static_cast<Json::UInt64>(mFlowControlCapacity->getOutboundCapacity());
    if (mFlowControlBytesCapacity)
    {
        if (mFlowControlBytesCapacity->getCapacity().mTotalCapacity)
        {
            res["local_capacity_bytes"]["reading"] = static_cast<Json::UInt64>(
                *(mFlowControlBytesCapacity->getCapacity().mTotalCapacity));
        }
        res["local_capacity_bytes"]["flood"] = static_cast<Json::UInt64>(
            mFlowControlBytesCapacity->getCapacity().mFloodCapacity);
        res["peer_capacity_bytes"] = static_cast<Json::UInt64>(
            mFlowControlBytesCapacity->getOutboundCapacity());
    }

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