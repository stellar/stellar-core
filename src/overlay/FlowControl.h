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

class AppConnector;
struct OverlayMetrics;

// num messages, bytes
using SendMoreCapacity = std::pair<uint64_t, uint64_t>;

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

// Flow control is a thread-safe class
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

    // Mutex to synchronize flow control state
    std::mutex mutable mFlowControlMutex;
    // Is this peer currently throttled due to lack of capacity
    std::optional<VirtualClock::time_point> mLastThrottle;

    NodeID mNodeID;
    FlowControlMessageCapacity mFlowControlCapacity;
    FlowControlByteCapacity mFlowControlBytesCapacity;

    OverlayMetrics& mOverlayMetrics;
    AppConnector& mAppConnector;
    bool const mUseBackgroundThread;

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

    bool hasOutboundCapacity(StellarMessage const& msg,
                             std::lock_guard<std::mutex>& lockGuard) const;
    virtual size_t
    getOutboundQueueByteLimit(std::lock_guard<std::mutex>& lockGuard) const;
    bool canRead(std::lock_guard<std::mutex> const& lockGuard) const;

  public:
    FlowControl(AppConnector& connector, bool useBackgoundThread);
    virtual ~FlowControl() = default;

    void maybeReleaseCapacity(StellarMessage const& msg);
    void handleTxSizeIncrease(uint32_t increase);
    // This method adds a new message to the outbound queue, while shedding
    // obsolete load
    void addMsgAndMaybeTrimQueue(std::shared_ptr<StellarMessage const> msg);
    // Return next batch of messages to send
    // NOTE: this methods _releases_ capacity and cleans up flow control queues
    std::vector<QueuedOutboundMessage> getNextBatchToSend();
    void updateMsgMetrics(std::shared_ptr<StellarMessage const> msg,
                          VirtualClock::time_point const& timePlaced);

#ifdef BUILD_TESTS
    FlowControlCapacity&
    getCapacity()
    {
        return mFlowControlCapacity;
    }

    FlowControlCapacity&
    getCapacityBytes()
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
    size_t
    getOutboundQueueByteLimit() const
    {
        std::lock_guard<std::mutex> lockGuard(mFlowControlMutex);
        return getOutboundQueueByteLimit(lockGuard);
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
    SendMoreCapacity endMessageProcessing(StellarMessage const& msg);
    bool canRead() const;

    // This method checks whether a peer has not requested new data within a
    // `timeout` (useful to diagnose if the connection is stuck for any reason)
    bool noOutboundCapacityTimeout(VirtualClock::time_point now,
                                   std::chrono::seconds timeout) const;

    Json::Value getFlowControlJsonInfo(bool compact) const;

    // Stores `peerID` to produce more useful log messages.
    void setPeerID(NodeID const& peerID);

    // Stop reading from this peer until capacity is released
    bool maybeThrottleRead();
    // After releasing capacity, check if throttling was applied, and if so,
    // reset it. Returns true if peer was throttled, and false otherwise
    bool stopThrottling();
    bool isThrottled() const;
};

}