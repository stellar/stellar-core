#pragma once

// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/StellarXDR.h"
#include "util/GlobalChecks.h"
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
        uint64_t mFloodCapacity{0};
        std::optional<uint64_t> mTotalCapacity;
    };

    // Capacity of local node configured by the operator
    ReadingCapacity mCapacity;
    // Capacity of a connected peer
    uint64_t mOutboundCapacity{0};

    NodeID const mNodeID;

    // Implementers should provide a way to derive how much capacity this
    // message will take
    virtual uint64_t getMsgResourceCount(StellarMessage const& msg) = 0;
    virtual ReadingCapacity getCapacityLimits() const = 0;

    void checkCapacityInvariants() const;

  public:
    static constexpr uint32_t MAX_FLOOD_MESSAGE_SIZE_BYTES = 64000;

    FlowControlCapacity(Application& app, NodeID peerID);
    virtual ~FlowControlCapacity(){};
    virtual void releaseOutboundCapacity(StellarMessage const& msg) = 0;
    virtual bool hasOutboundCapacity(StellarMessage const& msg) = 0;
    virtual bool hasReadingCapacity() = 0;
    void lockOutboundCapacity(StellarMessage const& msg);
    bool lockLocalCapacity(StellarMessage const& msg);
    // Release capacity used by this message. Return a struct that indicates how
    // much reading and flood capacity was freed
    uint64_t releaseLocalCapacity(StellarMessage const& msg);

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

#ifdef BUILD_TESTS
    void
    setOutboundCapacity(uint64_t newCapacity)
    {
        mOutboundCapacity = newCapacity;
    }
#endif
};

class FlowControlCapacityBytes : public FlowControlCapacity
{
    virtual ReadingCapacity getCapacityLimits() const override;

  public:
    FlowControlCapacityBytes(Application& app, NodeID nodeID);
    virtual ~FlowControlCapacityBytes(){};
    virtual uint64_t getMsgResourceCount(StellarMessage const& msg) override;
    virtual void releaseOutboundCapacity(StellarMessage const& msg) override;
    virtual bool hasOutboundCapacity(StellarMessage const& msg) override;
    virtual bool
    hasReadingCapacity() override
    {
        return true;
    }
};

class FlowControlCapacityMessages : public FlowControlCapacity
{
    virtual ReadingCapacity getCapacityLimits() const override;

  public:
    FlowControlCapacityMessages(Application& app, NodeID nodeID);
    virtual ~FlowControlCapacityMessages(){};
    virtual uint64_t getMsgResourceCount(StellarMessage const& msg) override;
    virtual void releaseOutboundCapacity(StellarMessage const& msg) override;
    virtual bool hasOutboundCapacity(StellarMessage const& msg) override;
    virtual bool
    hasReadingCapacity() override
    {
        checkCapacityInvariants();
        return *mCapacity.mTotalCapacity > 0;
    }
};
}