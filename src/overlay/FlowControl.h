#pragma once

// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/json/json.h"
#include "medida/timer.h"
#include "overlay/FlowControlCapacity.h"
#include "util/Timer.h"
#include <optional>

namespace stellar
{

class Peer;

// The FlowControl class allows core to throttle flood traffic among its
// connections. If a connections wants to use flow control, it should maintain
// an instance of this class, and use the following methods:
// * Inbound processing. Whenever a new message is received,
// begin/endMessageProcessing methods should be called to appropriately keep
// track of allowed capacity and potentially request more data from the
// connection.
// * Outbound processing. `sendMessage` will queue appropriate flood messages,
// and ensure that those are only sent when the receiver is ready to accept.
// This module also performs load shedding.
class FlowControl
{
  public:
    struct QueuedOutboundMessage
    {
        std::shared_ptr<StellarMessage const> mMessage;
        VirtualClock::time_point mTimeEmplaced;
    };

  private:
    struct FlowControlMetrics
    {
        FlowControlMetrics();
        medida::Timer mOutboundQueueDelaySCP;
        medida::Timer mOutboundQueueDelayTxs;
        medida::Timer mOutboundQueueDelayAdvert;
        medida::Timer mOutboundQueueDelayDemand;
    };

    // How many _hashes_ in total are queued?
    // NB: Each advert & demand contains a _vector_ of tx hashes.
    size_t mAdvertQueueTxHashCount{0};
    size_t mDemandQueueTxHashCount{0};
    size_t mTxQueueByteCount{0};

    NodeID mNodeID;
    std::shared_ptr<FlowControlCapacity> mFlowControlCapacity;
    std::shared_ptr<FlowControlByteCapacity> mFlowControlBytesCapacity;

    Application& mApp;

    // Outbound queues indexes by priority
    // Priority 0 - SCP messages
    // Priority 1 - transactions
    // Priority 2 - flood demands
    // Priority 3 - flood adverts
    std::array<std::deque<QueuedOutboundMessage>, 4> mOutboundQueues;

    // How many flood messages we received and processed since sending
    // SEND_MORE to this peer
    uint64_t mFloodDataProcessed{0};
    // How many bytes we received and processed since sending
    // SEND_MORE to this peer
    uint64_t mFloodDataProcessedBytes{0};
    std::optional<VirtualClock::time_point> mNoOutboundCapacity;
    FlowControlMetrics mMetrics;
    std::function<void(StellarMessage const&)> mSendCallback;

    // Release capacity used by this message. Return a struct that indicates how
    // much reading and flood capacity was freed
    void maybeSendNextBatch();
    // This methods drops obsolete load from the outbound queue
    void addMsgAndMaybeTrimQueue(std::shared_ptr<StellarMessage const> msg);
    void sendSendMore(uint32_t numMessages, std::shared_ptr<Peer> peer);
    void sendSendMore(uint32_t numMessages, uint32_t numBytes,
                      std::shared_ptr<Peer> peer);
    bool hasOutboundCapacity(StellarMessage const& msg) const;

  public:
    FlowControl(Application& app);
    virtual ~FlowControl() = default;

    virtual bool maybeSendMessage(std::shared_ptr<StellarMessage const> msg);
    void maybeReleaseCapacityAndTriggerSend(StellarMessage const& msg);
    virtual size_t getOutboundQueueByteLimit() const;
    void handleTxSizeIncrease(uint32_t increase, std::shared_ptr<Peer> peer);

#ifdef BUILD_TESTS
    std::shared_ptr<FlowControlCapacity>
    getCapacity() const
    {
        return mFlowControlCapacity;
    }

    std::shared_ptr<FlowControlCapacity>
    getCapacityBytes() const
    {
        return mFlowControlBytesCapacity;
    }

    void
    addToQueueAndMaybeTrimForTesting(std::shared_ptr<StellarMessage const> msg)
    {
        addMsgAndMaybeTrimQueue(msg);
    }

    std::array<std::deque<QueuedOutboundMessage>, 4>&
    getQueuesForTesting()
    {
        return mOutboundQueues;
    }

    void
    sendSendMoreForTesting(uint32_t numMessages, std::shared_ptr<Peer> peer)
    {
        sendSendMore(numMessages, peer);
    }
    void
    sendSendMoreForTesting(uint32_t numMessages, uint32_t numBytes,
                           std::shared_ptr<Peer> peer)
    {
        sendSendMore(numMessages, numBytes, peer);
    }

    size_t
    getTxQueueByteCountForTesting() const
    {
        return mTxQueueByteCount;
    }
    std::optional<size_t> mOutboundQueueLimit;
    void
    setOutboundQueueLimit(size_t bytes)
    {
        mOutboundQueueLimit = std::make_optional<size_t>(bytes);
    }
#endif

    static uint32_t getNumMessages(StellarMessage const& msg);
    bool isSendMoreValid(StellarMessage const& msg,
                         std::string& errorMsg) const;

    // This method ensures local capacity is locked now that we've received a
    // new message
    bool beginMessageProcessing(StellarMessage const& msg);

    // This method ensures local capacity is released now that we've finished
    // processing the message. It returns available capacity that can now be
    // requested from the peer.
    void endMessageProcessing(StellarMessage const& msg,
                              std::weak_ptr<Peer> peer);
    bool canRead() const;

    // This method return last timestamp (if any) when peer had no available
    // outbound capacity (useful to diagnose if the connection is stuck for any
    // reason)
    std::optional<VirtualClock::time_point>
    getOutboundCapacityTimestamp() const
    {
        return mNoOutboundCapacity;
    }

    Json::Value getFlowControlJsonInfo(bool compact) const;

    void start(std::weak_ptr<Peer> peer,
               std::function<void(StellarMessage const&)> sendCb,
               bool enableFCBytes);
};

}