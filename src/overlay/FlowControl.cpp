// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/FlowControl.h"
#include "herder/Herder.h"
#include "main/Application.h"
#include "medida/meter.h"
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
    std::lock_guard<std::recursive_mutex> guard(mFlowControlMutex);
#ifdef BUILD_TESTS
    if (mOutboundQueueLimit)
    {
        return *mOutboundQueueLimit;
    }
#endif
    return mAppConnector.getConfig().OUTBOUND_TX_QUEUE_BYTE_LIMIT;
}

FlowControl::FlowControl(OverlayAppConnector& connector,
                         bool useBackgroundThread)
    : mFlowControlCapacity(std::make_shared<FlowControlMessageCapacity>(
          connector.getConfig(), mNodeID))
    , mOverlayMetrics(connector.getOverlayManager().getOverlayMetrics())
    , mAppConnector(connector)
    , mUseBackgroundThread(useBackgroundThread)
    , mNoOutboundCapacity(
          std::make_optional<VirtualClock::time_point>(connector.now()))
{
    releaseAssert(threadIsMain());
}

bool
FlowControl::hasOutboundCapacity(StellarMessage const& msg) const
{
    releaseAssert(!threadIsMain() || !mUseBackgroundThread);
    std::lock_guard<std::recursive_mutex> guard(mFlowControlMutex);
    releaseAssert(mFlowControlCapacity);
    return mFlowControlCapacity->hasOutboundCapacity(msg) &&
           (!mFlowControlBytesCapacity ||
            mFlowControlBytesCapacity->hasOutboundCapacity(msg));
}

// Start flow control: send SEND_MORE to a peer to indicate available capacity
// NOTE: this is the only place where FlowControl has access to Peer via the
// lambda passed. The lambda calls `Peer::sendAuthenticatedMessage` which
// creates an opportunity for a deadlock if that method acquires mStateMutex.
// Peer must ensure it doesn't pass any lambda that acquires a lock.
void
FlowControl::start(
    NodeID const& peerID,
    std::function<void(std::shared_ptr<StellarMessage const>)> sendCb,
    std::optional<uint32_t> enableFCBytes)
{
    releaseAssert(threadIsMain());
    std::lock_guard<std::recursive_mutex> guard(mFlowControlMutex);
    mNodeID = peerID;
    mSendCallback = sendCb;

    if (enableFCBytes)
    {
        mFlowControlBytesCapacity = std::make_shared<FlowControlByteCapacity>(
            mAppConnector.getConfig(), mNodeID, *enableFCBytes);
    }
}

void
FlowControl::maybeReleaseCapacityAndTriggerSend(StellarMessage const& msg)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    std::lock_guard<std::recursive_mutex> guard(mFlowControlMutex);

    if (msg.type() == SEND_MORE || msg.type() == SEND_MORE_EXTENDED)
    {
        if (mNoOutboundCapacity)
        {
            mOverlayMetrics.mConnectionFloodThrottle.Update(
                mAppConnector.now() - *mNoOutboundCapacity);
        }
        mNoOutboundCapacity.reset();

        mFlowControlCapacity->releaseOutboundCapacity(msg);
        if (mFlowControlBytesCapacity)
        {
            mFlowControlBytesCapacity->releaseOutboundCapacity(msg);
        }

        CLOG_TRACE(Overlay, "{}: Peer {} sent {} ({} messages, {} bytes)",
                   mAppConnector.getConfig().toShortString(
                       mAppConnector.getConfig().NODE_SEED.getPublicKey()),
                   mAppConnector.getConfig().toShortString(mNodeID),
                   xdr::xdr_traits<MessageType>::enum_name(msg.type()),
                   getNumMessages(msg),
                   mFlowControlBytesCapacity
                       ? std::to_string(msg.sendMoreExtendedMessage().numBytes)
                       : "N/A");

        // SEND_MORE means we can free some capacity, and dump the next batch of
        // messages onto the writing queue
        if (mUseBackgroundThread)
        {
            mAppConnector.postOnOverlayThread(
                [this]() { this->maybeSendNextBatch(); },
                "FlowControl::maybeSendNextBatch");
        }
        else
        {
            maybeSendNextBatch();
        }
    }
}

void
FlowControl::maybeSendNextBatch()
{
    ZoneScoped;
    releaseAssert(!threadIsMain() || !mUseBackgroundThread);

    std::lock_guard<std::recursive_mutex> guard(mFlowControlMutex);

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
                CLOG_DEBUG(
                    Overlay, "{}: No outbound capacity for peer {}",
                    mAppConnector.getConfig().toShortString(
                        mAppConnector.getConfig().NODE_SEED.getPublicKey()),
                    mAppConnector.getConfig().toShortString(mNodeID));
                // Start a timeout for SEND_MORE
                mNoOutboundCapacity =
                    std::make_optional<VirtualClock::time_point>(
                        mAppConnector.now());
                break;
            }

            // TODO: ideally, there's no dependency on Peer here to avoid
            // deadlocks
            mSendCallback(front.mMessage);
            ++sent;
            auto& om = mOverlayMetrics;

            auto const& diff = mAppConnector.now() - front.mTimeEmplaced;
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
               mAppConnector.getConfig().toShortString(
                   mAppConnector.getConfig().NODE_SEED.getPublicKey()),
               mAppConnector.getConfig().toShortString(mNodeID), sent);
}

bool
FlowControl::maybeSendMessage(std::shared_ptr<StellarMessage const> msg)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    std::lock_guard<std::recursive_mutex> guard(mFlowControlMutex);

    if (OverlayManager::isFloodMessage(*msg))
    {
        addMsgAndMaybeTrimQueue(msg);
        if (mUseBackgroundThread)
        {
            mAppConnector.postOnOverlayThread(
                [this, msg]() {
                    std::lock_guard<std::recursive_mutex> guard(
                        mFlowControlMutex);
                    maybeSendNextBatch();
                },
                "FlowControl::maybeSendMessage");
        }
        else
        {
            maybeSendNextBatch();
        }
        return true;
    }
    return false;
}

void
FlowControl::handleTxSizeIncrease(uint32_t increase)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    std::lock_guard<std::recursive_mutex> guard(mFlowControlMutex);
    if (mFlowControlBytesCapacity)
    {
        releaseAssert(increase > 0);
        // Bump flood capacity to accommodate the upgrade
        mFlowControlBytesCapacity->handleTxSizeIncrease(increase);
    }
}

bool
FlowControl::beginMessageProcessing(StellarMessage const& msg)
{
    ZoneScoped;
    releaseAssert(!threadIsMain() || !mUseBackgroundThread);
    std::lock_guard<std::recursive_mutex> guard(mFlowControlMutex);

    return mFlowControlCapacity->lockLocalCapacity(msg) &&
           (!mFlowControlBytesCapacity ||
            mFlowControlBytesCapacity->lockLocalCapacity(msg));
}

SendMoreCapacity
FlowControl::endMessageProcessing(StellarMessage const& msg)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    std::lock_guard<std::recursive_mutex> guard(mFlowControlMutex);

    mFloodDataProcessed += mFlowControlCapacity->releaseLocalCapacity(msg);
    if (mFlowControlBytesCapacity)
    {
        mFloodDataProcessedBytes +=
            mFlowControlBytesCapacity->releaseLocalCapacity(msg);
    }

    releaseAssert(mFloodDataProcessed <=
                  mAppConnector.getConfig().FLOW_CONTROL_SEND_MORE_BATCH_SIZE);
    bool shouldSendMore =
        mFloodDataProcessed ==
        mAppConnector.getConfig().FLOW_CONTROL_SEND_MORE_BATCH_SIZE;
    if (mFlowControlBytesCapacity)
    {
        auto const byteBatchSize = mAppConnector.getOverlayManager()
                                       .getFlowControlBytesConfig()
                                       .mBatchSize;
        shouldSendMore =
            shouldSendMore || mFloodDataProcessedBytes >= byteBatchSize;
    }

    SendMoreCapacity res{0, std::nullopt};
    if (shouldSendMore)
    {
        // First save result to return
        res.first = mFloodDataProcessed;
        if (mFlowControlBytesCapacity)
        {
            res.second = mFloodDataProcessedBytes;
        }

        // Reset counters
        mFloodDataProcessed = 0;
        mFloodDataProcessedBytes = 0;
    }

    return res;
}

bool
FlowControl::canRead() const
{
    std::lock_guard<std::recursive_mutex> guard(mFlowControlMutex);
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
    releaseAssert(threadIsMain());
    std::lock_guard<std::recursive_mutex> guard(mFlowControlMutex);

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
    releaseAssert(threadIsMain());
    auto const& msg = *(queuedMsg.mMessage);
    bool dropType = msg.type() == TRANSACTION || msg.type() == FLOOD_ADVERT ||
                    msg.type() == FLOOD_DEMAND;
    return dropType && (now - queuedMsg.mTimeEmplaced > OUTBOUND_QUEUE_TIMEOUT);
}

void
FlowControl::addMsgAndMaybeTrimQueue(std::shared_ptr<StellarMessage const> msg)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    std::lock_guard<std::recursive_mutex> guard(mFlowControlMutex);
    releaseAssert(msg);
    auto type = msg->type();
    size_t msgQInd = 0;
    auto now = mAppConnector.now();

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
            if (bytes > mAppConnector.getHerder().getMaxTxSize())
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

    queue.emplace_back(QueuedOutboundMessage{msg, mAppConnector.now()});

    size_t dropped = 0;

    uint32_t const limit =
        mAppConnector.getLedgerManager().getLastMaxTxSetSizeOps();
    auto& om = mOverlayMetrics;
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
        auto minSlotToRemember =
            mAppConnector.getHerder().getMinLedgerSeqToRemember();
        auto checkpointSeq =
            mAppConnector.getHerder().getMostRecentCheckpointSeq();
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
                     mAppConnector.getHerder().isNewerNominationOrBallotSt(
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
                   mAppConnector.getConfig().toShortString(mNodeID));
    }
}

Json::Value
FlowControl::getFlowControlJsonInfo(bool compact) const
{
    releaseAssert(threadIsMain());
    std::lock_guard<std::recursive_mutex> guard(mFlowControlMutex);

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

void
FlowControl::throttleRead()
{
    std::lock_guard<std::recursive_mutex> guard(mFlowControlMutex);
    CLOG_DEBUG(Overlay, "Throttle reading from peer {}",
               mAppConnector.getConfig().toShortString(mNodeID));
    mLastThrottle = mAppConnector.now();
}

bool
FlowControl::stopThrottling()
{
    std::lock_guard<std::recursive_mutex> guard(mFlowControlMutex);
    releaseAssert(threadIsMain());
    if (mLastThrottle)
    {
        CLOG_DEBUG(Overlay, "Stop throttling reading from peer {}",
                   mAppConnector.getConfig().toShortString(mNodeID));
        mOverlayMetrics.mConnectionReadThrottle.Update(mAppConnector.now() -
                                                       *mLastThrottle);
        mLastThrottle.reset();
        return true;
    }
    return false;
}

bool
FlowControl::isThrottled() const
{
    std::lock_guard<std::recursive_mutex> guard(mFlowControlMutex);
    return static_cast<bool>(mLastThrottle);
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
    releaseAssert(threadIsMain());
}
}