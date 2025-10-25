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
#include "overlay/OverlayUtils.h"
#include "util/Logging.h"
#include <Tracy.hpp>

namespace stellar
{

size_t
FlowControl::getOutboundQueueByteLimit(MutexLocker& lockGuard) const
{
#ifdef BUILD_TESTS
    if (mOutboundQueueLimit)
    {
        return *mOutboundQueueLimit;
    }
#endif
    return mAppConnector.getConfig().OUTBOUND_TX_QUEUE_BYTE_LIMIT;
}

FlowControl::FlowControl(AppConnector& connector, bool useBackgroundThread)
    : mFlowControlCapacity(connector.getConfig(), mNodeID)
    , mFlowControlBytesCapacity(
          connector.getConfig(), mNodeID,
          connector.getOverlayManager().getFlowControlBytesTotal())
    , mOverlayMetrics(connector.getOverlayManager().getOverlayMetrics())
    , mAppConnector(connector)
    , mUseBackgroundThread(useBackgroundThread)
    , mNoOutboundCapacity(
          std::make_optional<VirtualClock::time_point>(connector.now()))
{
    releaseAssert(threadIsMain());
}

bool
FlowControl::hasOutboundCapacity(StellarMessage const& msg,
                                 MutexLocker& lockGuard) const
{
    releaseAssert(!threadIsMain() || !mUseBackgroundThread);
    return mFlowControlCapacity.hasOutboundCapacity(msg) &&
           mFlowControlBytesCapacity.hasOutboundCapacity(msg);
}

bool
FlowControl::noOutboundCapacityTimeout(VirtualClock::time_point now,
                                       std::chrono::seconds timeout) const
{
    MutexLocker guard(mFlowControlMutex);
    return mNoOutboundCapacity && now - *mNoOutboundCapacity >= timeout;
}

void
FlowControl::setPeerID(NodeID const& peerID)
{
    releaseAssert(threadIsMain());
    MutexLocker guard(mFlowControlMutex);
    mNodeID = peerID;
}

void
FlowControl::maybeReleaseCapacity(StellarMessage const& msg)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    MutexLocker guard(mFlowControlMutex);

    if (msg.type() == SEND_MORE_EXTENDED)
    {
        if (mNoOutboundCapacity)
        {
            mOverlayMetrics.mConnectionFloodThrottle.Update(
                mAppConnector.now() - *mNoOutboundCapacity);
        }
        mNoOutboundCapacity.reset();

        mFlowControlCapacity.releaseOutboundCapacity(msg);
        mFlowControlBytesCapacity.releaseOutboundCapacity(msg);

        CLOG_TRACE(Overlay, "{}: Peer {} sent {} ({} messages, {} bytes)",
                   mAppConnector.getConfig().toShortString(
                       mAppConnector.getConfig().NODE_SEED.getPublicKey()),
                   mAppConnector.getConfig().toShortString(mNodeID),
                   xdr::xdr_traits<MessageType>::enum_name(msg.type()),
                   getNumMessages(msg),
                   std::to_string(msg.sendMoreExtendedMessage().numBytes));
    }
}

void
FlowControl::processSentMessages(
    FloodQueues<ConstStellarMessagePtr> const& sentMessages)
{
    ZoneScoped;
    releaseAssert(!threadIsMain() || !mUseBackgroundThread);

    MutexLocker guard(mFlowControlMutex);
    for (int i = 0; i < sentMessages.size(); i++)
    {
        auto const& sentMsgs = sentMessages[i];
        auto& queue = mOutboundQueues[i];

        for (auto const& item : sentMsgs)
        {
            if (queue.empty() || item != queue.front().mMessage)
            {
                // queue got cleaned up from the front, skip this queue
                continue;
            }

            auto& front = queue.front();
            switch (front.mMessage->type())
            {
#ifdef BUILD_TESTS
            case TX_SET:
#endif
            case TRANSACTION:
            {
                releaseAssert(OverlayManager::isFloodMessage(*front.mMessage));
                size_t s = mFlowControlBytesCapacity.getMsgResourceCount(
                    *front.mMessage);
                releaseAssert(mTxQueueByteCount >= s);
                mTxQueueByteCount -= s;
            }
            break;
            case SCP_MESSAGE:
                break;
            case FLOOD_DEMAND:
            {
                size_t s = front.mMessage->floodDemand().txHashes.size();
                releaseAssert(mDemandQueueTxHashCount >= s);
                mDemandQueueTxHashCount -= s;
            }
            break;
            case FLOOD_ADVERT:
            {
                size_t s = front.mMessage->floodAdvert().txHashes.size();
                releaseAssert(mAdvertQueueTxHashCount >= s);
                mAdvertQueueTxHashCount -= s;
            }
            break;
            default:
            {
                throw std::runtime_error(
                    "Unknown message type in processSentMessages");
            }
            }
            queue.pop_front();
        }
    }
}

std::vector<FlowControl::QueuedOutboundMessage>
FlowControl::getNextBatchToSend()
{
    ZoneScoped;
    releaseAssert(!threadIsMain() || !mUseBackgroundThread);

    MutexLocker guard(mFlowControlMutex);
    std::vector<QueuedOutboundMessage> batchToSend;

    int sent = 0;
    for (auto& queue : mOutboundQueues)
    {
        for (auto& outboundMsg : queue)
        {
            auto const& msg = *(outboundMsg.mMessage);
            // Can't send _current_ message
            if (!hasOutboundCapacity(msg, guard))
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

            if (outboundMsg.mBeingSent)
            {
                // Already sent
                continue;
            }

            batchToSend.push_back(outboundMsg);
            outboundMsg.mBeingSent = true;
            ++sent;

            mFlowControlCapacity.lockOutboundCapacity(msg);
            mFlowControlBytesCapacity.lockOutboundCapacity(msg);

            // Do not pop messages here, cleanup after the call to async_write
            // (its write handler invokes processSentMessages)
        }
    }

    CLOG_TRACE(Overlay, "{} Peer {}: send next flood batch of {}",
               mAppConnector.getConfig().toShortString(
                   mAppConnector.getConfig().NODE_SEED.getPublicKey()),
               mAppConnector.getConfig().toShortString(mNodeID), sent);
    return batchToSend;
}

void
FlowControl::updateMsgMetrics(std::shared_ptr<StellarMessage const> msg,
                              VirtualClock::time_point const& timePlaced)
{
    // The lock isn't strictly needed here, but is added for consistency and
    // future-proofing this function
    MutexLocker guard(mFlowControlMutex);
    auto diff = mAppConnector.now() - timePlaced;

    auto updateQueueDelay = [&](auto& queue, auto& metrics) {
        queue.Update(diff);
        metrics.Update(diff);
    };

    auto& om = mAppConnector.getOverlayMetrics();
    switch (msg->type())
    {
    case TRANSACTION:
#ifdef BUILD_TESTS
    case TX_SET:
#endif
        releaseAssert(OverlayManager::isFloodMessage(*msg));
        updateQueueDelay(om.mOutboundQueueDelayTxs,
                         mMetrics.mOutboundQueueDelayTxs);
        break;
    case SCP_MESSAGE:
        updateQueueDelay(om.mOutboundQueueDelaySCP,
                         mMetrics.mOutboundQueueDelaySCP);
        break;
    case FLOOD_DEMAND:
        updateQueueDelay(om.mOutboundQueueDelayDemand,
                         mMetrics.mOutboundQueueDelayDemand);
        break;
    case FLOOD_ADVERT:
        updateQueueDelay(om.mOutboundQueueDelayAdvert,
                         mMetrics.mOutboundQueueDelayAdvert);
        break;
    default:
    {
        logErrorOrThrow(
            fmt::format("Unknown message type {} in updateMsgMetrics",
                        static_cast<int>(msg->type())));
    }
    }
}

void
FlowControl::handleTxSizeIncrease(uint32_t increase)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    MutexLocker guard(mFlowControlMutex);
    releaseAssert(increase > 0);
    // Bump flood capacity to accommodate the upgrade
    mFlowControlBytesCapacity.handleTxSizeIncrease(increase);
}

bool
FlowControl::beginMessageProcessing(StellarMessage const& msg)
{
    ZoneScoped;
    releaseAssert(!threadIsMain() || !mUseBackgroundThread);
    MutexLocker guard(mFlowControlMutex);

    return mFlowControlCapacity.lockLocalCapacity(msg) &&
           mFlowControlBytesCapacity.lockLocalCapacity(msg);
}

SendMoreCapacity
FlowControl::endMessageProcessing(StellarMessage const& msg)
{
    ZoneScoped;
    MutexLocker guard(mFlowControlMutex);

    mFloodDataProcessed += mFlowControlCapacity.releaseLocalCapacity(msg);
    mFloodDataProcessedBytes +=
        mFlowControlBytesCapacity.releaseLocalCapacity(msg);
    mTotalMsgsProcessed++;

    releaseAssert(mFloodDataProcessed <=
                  mAppConnector.getConfig().FLOW_CONTROL_SEND_MORE_BATCH_SIZE);
    bool shouldSendMore =
        mFloodDataProcessed ==
        mAppConnector.getConfig().FLOW_CONTROL_SEND_MORE_BATCH_SIZE;
    auto const byteBatchSize =
        OverlayManager::getFlowControlBytesBatch(mAppConnector.getConfig());
    shouldSendMore =
        shouldSendMore || mFloodDataProcessedBytes >= byteBatchSize;

    SendMoreCapacity res{0, 0, 0};
    if (mTotalMsgsProcessed == mAppConnector.getConfig().PEER_READING_CAPACITY)
    {
        res.numTotalMessages = mTotalMsgsProcessed;
        mTotalMsgsProcessed = 0;
    }

    if (shouldSendMore)
    {
        // First save result to return
        res.numFloodMessages = mFloodDataProcessed;
        res.numFloodBytes = mFloodDataProcessedBytes;

        // Reset counters
        mFloodDataProcessed = 0;
        mFloodDataProcessedBytes = 0;
    }

    return res;
}

bool
FlowControl::canRead(MutexLocker const& guard) const
{
    return mFlowControlBytesCapacity.canRead() &&
           mFlowControlCapacity.canRead();
}

bool
FlowControl::canRead() const
{
    MutexLocker guard(mFlowControlMutex);
    return canRead(guard);
}

uint32_t
FlowControl::getNumMessages(StellarMessage const& msg)
{
    releaseAssert(msg.type() == SEND_MORE_EXTENDED);
    return msg.sendMoreExtendedMessage().numMessages;
}

uint32_t
FlowControl::getMessagePriority(StellarMessage const& msg)
{
    switch (msg.type())
    {
    case SCP_MESSAGE:
        return 0;
    case TRANSACTION:
#ifdef BUILD_TESTS
    case TX_SET:
#endif
        releaseAssert(OverlayManager::isFloodMessage(msg));
        return 1;
    case FLOOD_DEMAND:
        return 2;
    case FLOOD_ADVERT:
        return 3;
    default:
    {
        throw std::runtime_error("Unknown message type in getMessagePriority");
    }
    }
}

bool
FlowControl::isSendMoreValid(StellarMessage const& msg,
                             std::string& errorMsg) const
{
    releaseAssert(threadIsMain());
    MutexLocker guard(mFlowControlMutex);

    if (msg.type() != SEND_MORE_EXTENDED)
    {
        errorMsg =
            fmt::format("unexpected message type {}",
                        xdr::xdr_traits<MessageType>::enum_name(msg.type()));
        return false;
    }

    // If flow control in bytes isn't enabled, SEND_MORE must have non-zero
    // messages. If flow control in bytes is enabled, SEND_MORE_EXTENDED must
    // have non-zero bytes, but _can_ have 0 messages to support upgrades
    if (msg.sendMoreExtendedMessage().numBytes == 0)
    {
        errorMsg =
            fmt::format("invalid message {}",
                        xdr::xdr_traits<MessageType>::enum_name(msg.type()));
        return false;
    }

    auto overflow =
        getNumMessages(msg) >
            (UINT64_MAX - mFlowControlCapacity.getOutboundCapacity()) ||
        msg.sendMoreExtendedMessage().numBytes >
            (UINT64_MAX - mFlowControlBytesCapacity.getOutboundCapacity());
    if (overflow)
    {
        errorMsg = "Peer capacity overflow";
        return false;
    }
    return true;
}

void
FlowControl::addMsgAndMaybeTrimQueue(std::shared_ptr<StellarMessage const> msg)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    MutexLocker guard(mFlowControlMutex);
    releaseAssert(msg);
    auto type = msg->type();
    size_t msgQInd = 0;

    switch (type)
    {
    case SCP_MESSAGE:
    {
        msgQInd = 0;
    }
    break;
    case TRANSACTION:
#ifdef BUILD_TESTS
    case TX_SET:
#endif
    {
        releaseAssert(OverlayManager::isFloodMessage(*msg));
        msgQInd = 1;
        auto bytes = mFlowControlBytesCapacity.getMsgResourceCount(*msg);
        // Don't accept transactions that are over allowed byte limit: those
        // won't be properly flooded anyways
        if (bytes > mAppConnector.getHerder().getMaxTxSize())
        {
            return;
        }
        mTxQueueByteCount += bytes;
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
    {
        throw std::runtime_error(
            "Unknown message type in addMsgAndMaybeTrimQueue");
    }
    }
    auto& queue = mOutboundQueues[msgQInd];

    queue.emplace_back(QueuedOutboundMessage{msg, mAppConnector.now()});

    size_t dropped = 0;

    auto& lm = mAppConnector.getLedgerManager();
    uint32_t limit = lm.getLastMaxTxSetSizeOps();

    if (protocolVersionStartsFrom(
            lm.getLastClosedLedgerHeader().header.ledgerVersion,
            SOROBAN_PROTOCOL_VERSION))
    {
        limit = saturatingAdd(
            limit, lm.getLastClosedSorobanNetworkConfig().ledgerMaxTxCount());
    }
    auto& om = mOverlayMetrics;
    if (type == TRANSACTION)
    {
        bool isOverLimit = queue.size() > limit ||
                           mTxQueueByteCount > getOutboundQueueByteLimit(guard);

        // If we are at limit, we're probably really behind, so drop the entire
        // queue
        if (isOverLimit)
        {
            dropped = queue.size();
            mTxQueueByteCount = 0;
            queue.clear();
            om.mOutboundQueueDropTxs.Mark(dropped);
        }
    }
    // When at limit, do not drop SCP messages, critical to consensus
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
            // Already being sent, skip
            if (it->mBeingSent)
            {
                ++it;
                continue;
            }

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
                releaseAssert(!queue.back().mBeingSent);
                releaseAssert(!it->mBeingSent);
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
        if (mAdvertQueueTxHashCount > limit)
        {
            dropped = mAdvertQueueTxHashCount;
            mAdvertQueueTxHashCount = 0;
            queue.clear();
            om.mOutboundQueueDropAdvert.Mark(dropped);
        }
    }
    else if (type == FLOOD_DEMAND)
    {
        if (mDemandQueueTxHashCount > limit)
        {
            dropped = mDemandQueueTxHashCount;
            mDemandQueueTxHashCount = 0;
            queue.clear();
            om.mOutboundQueueDropDemand.Mark(dropped);
        }
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
    MutexLocker guard(mFlowControlMutex);

    Json::Value res;
    if (mFlowControlCapacity.getCapacity().mTotalCapacity)
    {
        res["local_capacity"]["reading"] = static_cast<Json::UInt64>(
            *(mFlowControlCapacity.getCapacity().mTotalCapacity));
    }
    res["local_capacity"]["flood"] = static_cast<Json::UInt64>(
        mFlowControlCapacity.getCapacity().mFloodCapacity);
    res["peer_capacity"] =
        static_cast<Json::UInt64>(mFlowControlCapacity.getOutboundCapacity());
    if (mFlowControlBytesCapacity.getCapacity().mTotalCapacity)
    {
        res["local_capacity_bytes"]["reading"] = static_cast<Json::UInt64>(
            *(mFlowControlBytesCapacity.getCapacity().mTotalCapacity));
    }
    res["local_capacity_bytes"]["flood"] = static_cast<Json::UInt64>(
        mFlowControlBytesCapacity.getCapacity().mFloodCapacity);
    res["peer_capacity_bytes"] = static_cast<Json::UInt64>(
        mFlowControlBytesCapacity.getOutboundCapacity());

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

bool
FlowControl::maybeThrottleRead()
{
    MutexLocker guard(mFlowControlMutex);
    if (!canRead(guard))
    {
        CLOG_DEBUG(Overlay, "Throttle reading from peer {}",
                   mAppConnector.getConfig().toShortString(mNodeID));
        mLastThrottle = mAppConnector.now();
        return true;
    }
    return false;
}

void
FlowControl::stopThrottling()
{
    MutexLocker guard(mFlowControlMutex);
    releaseAssert(mLastThrottle);
    CLOG_DEBUG(Overlay, "Stop throttling reading from peer {}",
               mAppConnector.getConfig().toShortString(mNodeID));
    mOverlayMetrics.mConnectionReadThrottle.Update(mAppConnector.now() -
                                                   *mLastThrottle);
    mLastThrottle.reset();
}

bool
FlowControl::isThrottled() const
{
    MutexLocker guard(mFlowControlMutex);
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
