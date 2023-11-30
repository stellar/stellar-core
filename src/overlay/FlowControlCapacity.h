#pragma once

// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/StellarXDR.h"
#include <optional>

namespace stellar
{

class Application;

class FlowControlCapacity
{
  protected:
    Application& mApp;

    struct ReadingCapacity
    {
        uint64_t mFloodCapacity;
        std::optional<uint64_t> mTotalCapacity;
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
    bool lockLocalCapacity(StellarMessage const& msg);
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

    FlowControlCapacity(Application& app, NodeID const& nodeID);
};

class FlowControlByteCapacity : public FlowControlCapacity
{
    // FlowControlByteCapacity capacity limits may change due to protocol
    // upgrades
    ReadingCapacity mCapacityLimits;

  public:
    FlowControlByteCapacity(Application& app, NodeID const& nodeID);
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
    FlowControlMessageCapacity(Application& app, NodeID const& nodeID);
    virtual ~FlowControlMessageCapacity() = default;
    virtual uint64_t
    getMsgResourceCount(StellarMessage const& msg) const override;
    virtual ReadingCapacity getCapacityLimits() const override;
    void releaseOutboundCapacity(StellarMessage const& msg) override;
    bool canRead() const override;
};
}