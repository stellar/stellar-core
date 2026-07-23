// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "main/Config.h"
#include "overlay/Peer.h"
#include "overlay/TCPPeer.h"
#include <optional>

namespace stellar
{

struct StellarMessage;

// FlowControlCapacity is _not_ thread-safe; users (e.g. FlowControl) must
// implement synchronization.
class FlowControlCapacity
{
  protected:
    Config const mConfig;

    struct ReadingCapacity
    {
        uint64_t mFloodCapacity;
        uint64_t mTotalCapacity;
    };

    // Capacity of local node configured by the operator
    ReadingCapacity mCapacity;

    // Capacity of a connected peer
    uint64_t mOutboundCapacity{0};
    NodeID const& mNodeID;

  public:
    virtual uint64_t getMsgResourceCount(StellarMessage const& msg) const = 0;
    virtual ReadingCapacity getCapacityLimits() const = 0;
    virtual void releaseOutboundCapacity(StellarMessage const& msg) = 0;

    void lockOutboundCapacity(StellarMessage const& msg);
    void lockLocalCapacity(StellarMessage const& msg);
    bool canLockLocalCapacity(StellarMessage const& msg) const;
    // Release capacity used by this message. Return how flood capacity was
    // freed
    uint64_t releaseLocalCapacity(StellarMessage const& msg);

    bool hasOutboundCapacity(StellarMessage const& msg) const;
    void checkCapacityInvariants() const;
    ReadingCapacity
    getCapacity() const
    {
        return mCapacity;
    }

    uint64_t
    getOutboundCapacity() const
    {
        return mOutboundCapacity;
    }

    virtual bool canRead() const = 0;

    static uint64_t msgBodySize(StellarMessage const& msg);

#ifdef BUILD_TESTS
    void
    setOutboundCapacity(uint64_t newCapacity)
    {
        mOutboundCapacity = newCapacity;
    }
#endif

    FlowControlCapacity(Config const& cfg, NodeID const& nodeID);
};

class FlowControlByteCapacity : public FlowControlCapacity
{
    // FlowControlByteCapacity capacity limits may change due to protocol
    // upgrades
    ReadingCapacity mCapacityLimits;

  public:
    // Subtle: when we issue a read, want to deduct the size of the read from
    // the capacity and stop when we "hit zero"; but we won't know how big the
    // read is before we perform it. It could be anywhere up to MAX_MESSAGE_SIZE
    // (plus 4 for the XDR header bytes) and so we treat that as an artificial
    // floor for the capacity value -- a sort of "virtual zero point" -- and
    // only allow reads when we have capacity _greater_ than that floor, such
    // that we know any read won't underflow true 0 and wrap around. The other
    // option would be to use a signed integer for capacity and tolerate
    // negative capacities sometimes, which would be confusing/alarming in a
    // different way. We decided this was nicer.
    uint64_t static constexpr BYTE_CAPACITY_READ_FLOOR = MAX_MESSAGE_SIZE + 4;

    FlowControlByteCapacity(Config const& cfg, NodeID const& nodeID,
                            uint32_t floodCapacity);
    virtual ~FlowControlByteCapacity() = default;
    virtual uint64_t
    getMsgResourceCount(StellarMessage const& msg) const override;
    virtual ReadingCapacity getCapacityLimits() const override;
    virtual void releaseOutboundCapacity(StellarMessage const& msg) override;
    bool canRead() const override;
    void handleTxSizeIncrease(uint32_t increase);
};

class FlowControlMessageCapacity : public FlowControlCapacity
{
  public:
    FlowControlMessageCapacity(Config const& cfg, NodeID const& nodeID);
    virtual ~FlowControlMessageCapacity() = default;
    virtual uint64_t
    getMsgResourceCount(StellarMessage const& msg) const override;
    virtual ReadingCapacity getCapacityLimits() const override;
    void releaseOutboundCapacity(StellarMessage const& msg) override;
    bool canRead() const override;
};
}
