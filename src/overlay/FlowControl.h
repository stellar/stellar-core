#pragma once

// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/json/json.h"
#include "medida/timer.h"
#include "overlay/FlowControlCapacity.h"
#include "util/ThreadAnnotations.h"
#include "util/Timer.h"
#include <optional>

namespace stellar
{

class AppConnector;
struct OverlayMetrics;

struct SendMoreCapacity
{
    uint64_t numFloodMessages{0};
    uint64_t numFloodBytes{0};
    uint32_t numTotalMessages{0};
};

template <typename T> using FloodQueues = typename std::array<std::deque<T>, 4>;
using ConstStellarMessagePtr = std::shared_ptr<StellarMessage const>;

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
        ConstStellarMessagePtr mMessage;
        VirtualClock::time_point mTimeEmplaced;
        // Is the message currently being sent (for async write flows)
        bool mBeingSent{false};
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
    size_t mAdvertQueueTxHashCount GUARDED_BY(mFlowControlMutex){0};
    size_t mDemandQueueTxHashCount GUARDED_BY(mFlowControlMutex){0};
    size_t mTxQueueByteCount GUARDED_BY(mFlowControlMutex){0};

    // Mutex to synchronize flow control state
    Mutex mutable mFlowControlMutex;
    // Is this peer currently throttled due to lack of capacity
    std::optional<VirtualClock::time_point>
        mLastThrottle GUARDED_BY(mFlowControlMutex);

    NodeID mNodeID GUARDED_BY(mFlowControlMutex);
    FlowControlMessageCapacity
        mFlowControlCapacity GUARDED_BY(mFlowControlMutex);
    FlowControlByteCapacity
        mFlowControlBytesCapacity GUARDED_BY(mFlowControlMutex);

    OverlayMetrics& mOverlayMetrics;
    AppConnector& mAppConnector;
    bool const mUseBackgroundThread;

    // Outbound queues indexes by priority
    // Priority 0 - SCP messages
    // Priority 1 - transactions
    // Priority 2 - flood demands
    // Priority 3 - flood adverts
    FloodQueues<QueuedOutboundMessage>
        mOutboundQueues GUARDED_BY(mFlowControlMutex);

    // How many flood messages we received and processed since sending
    // SEND_MORE to this peer
    uint64_t mFloodDataProcessed GUARDED_BY(mFlowControlMutex){0};
    // How many bytes we received and processed since sending
    // SEND_MORE to this peer
    uint64_t mFloodDataProcessedBytes GUARDED_BY(mFlowControlMutex){0};
    // How many total messages we received and processed so far (used to track
    // throttling)
    uint64_t mTotalMsgsProcessed GUARDED_BY(mFlowControlMutex){0};
    std::optional<VirtualClock::time_point>
        mNoOutboundCapacity GUARDED_BY(mFlowControlMutex);
    FlowControlMetrics mMetrics GUARDED_BY(mFlowControlMutex);

    bool hasOutboundCapacity(StellarMessage const& msg,
                             MutexLocker& lockGuard) const
        REQUIRES(mFlowControlMutex);
    virtual size_t getOutboundQueueByteLimit(MutexLocker& lockGuard) const
        REQUIRES(mFlowControlMutex);
    bool canRead(MutexLocker const& lockGuard) const
        REQUIRES(mFlowControlMutex);

  public:
    FlowControl(AppConnector& connector, bool useBackgoundThread);
    virtual ~FlowControl() = default;

    void maybeReleaseCapacity(StellarMessage const& msg)
        LOCKS_EXCLUDED(mFlowControlMutex);
    void handleTxSizeIncrease(uint32_t increase)
        LOCKS_EXCLUDED(mFlowControlMutex);
    // This method adds a new message to the outbound queue, while shedding
    // obsolete load
    void addMsgAndMaybeTrimQueue(std::shared_ptr<StellarMessage const> msg)
        LOCKS_EXCLUDED(mFlowControlMutex);
    // Return next batch of messages to send
    // NOTE: this method consumes outbound capacity of the receiving peer
    std::vector<QueuedOutboundMessage> getNextBatchToSend()
        LOCKS_EXCLUDED(mFlowControlMutex);
    void updateMsgMetrics(std::shared_ptr<StellarMessage const> msg,
                          VirtualClock::time_point const& timePlaced)
        LOCKS_EXCLUDED(mFlowControlMutex);

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

    FloodQueues<QueuedOutboundMessage>&
    getQueuesForTesting()
    {
        return mOutboundQueues;
    }

    size_t
    getTxQueueByteCountForTesting() const
    {
        return mTxQueueByteCount;
    }
    std::optional<size_t> mOutboundQueueLimit GUARDED_BY(mFlowControlMutex);
    void
    setOutboundQueueLimit(size_t bytes)
    {
        mOutboundQueueLimit = std::make_optional<size_t>(bytes);
    }
    size_t
    getOutboundQueueByteLimit() const
    {
        MutexLocker lockGuard(mFlowControlMutex);
        return getOutboundQueueByteLimit(lockGuard);
    }
#endif

    static uint32_t getNumMessages(StellarMessage const& msg);
    static uint32_t getMessagePriority(StellarMessage const& msg);
    bool isSendMoreValid(StellarMessage const& msg, std::string& errorMsg) const
        LOCKS_EXCLUDED(mFlowControlMutex);

    // This method ensures local capacity is locked now that we've received a
    // new message
    bool beginMessageProcessing(StellarMessage const& msg)
        LOCKS_EXCLUDED(mFlowControlMutex);

    // This method ensures local capacity is released now that we've finished
    // processing the message. It returns available capacity that can now be
    // requested from the peer.
    SendMoreCapacity endMessageProcessing(StellarMessage const& msg)
        LOCKS_EXCLUDED(mFlowControlMutex);
    bool canRead() const LOCKS_EXCLUDED(mFlowControlMutex);

    // This method checks whether a peer has not requested new data within a
    // `timeout` (useful to diagnose if the connection is stuck for any reason)
    bool noOutboundCapacityTimeout(VirtualClock::time_point now,
                                   std::chrono::seconds timeout) const
        LOCKS_EXCLUDED(mFlowControlMutex);

    Json::Value getFlowControlJsonInfo(bool compact) const
        LOCKS_EXCLUDED(mFlowControlMutex);

    // Stores `peerID` to produce more useful log messages.
    void setPeerID(NodeID const& peerID) LOCKS_EXCLUDED(mFlowControlMutex);

    // Stop reading from this peer until capacity is released
    bool maybeThrottleRead() LOCKS_EXCLUDED(mFlowControlMutex);
    // After releasing capacity, check if throttling was applied, and if so,
    // reset it. Returns true if peer was throttled, and false otherwise
    void stopThrottling() LOCKS_EXCLUDED(mFlowControlMutex);
    bool isThrottled() const LOCKS_EXCLUDED(mFlowControlMutex);

    // A function to be called once a batch of messages is sent (typically, this
    // is called once async_write completes and invokes a handler that calls
    // this function). This function will appropriatly trim outbound queues and
    // release capacity used by the messages that were sent.
    void
    processSentMessages(FloodQueues<ConstStellarMessagePtr> const& sentMessages)
        LOCKS_EXCLUDED(mFlowControlMutex);
};

}