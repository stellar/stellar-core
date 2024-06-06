// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/FlowControlCapacity.h"
#include "overlay/FlowControl.h"
#include "overlay/OverlayManager.h"
#include "util/Logging.h"
#include <Tracy.hpp>

namespace stellar
{

FlowControlMessageCapacity::FlowControlMessageCapacity(Config const& cfg,
                                                       NodeID const& nodeID)
    : FlowControlCapacity(cfg, nodeID)
{
    mCapacity = getCapacityLimits();
}

uint64_t
FlowControlMessageCapacity::getMsgResourceCount(StellarMessage const& msg) const
{
    // Each message takes one unit of capacity
    return 1;
}

FlowControlCapacity::ReadingCapacity
FlowControlMessageCapacity::getCapacityLimits() const
{
    return {mConfig.PEER_FLOOD_READING_CAPACITY,
            std::make_optional<uint64_t>(mConfig.PEER_READING_CAPACITY)};
}

void
FlowControlMessageCapacity::releaseOutboundCapacity(StellarMessage const& msg)
{
    ZoneScoped;
    releaseAssert(msg.type() == SEND_MORE_EXTENDED);
    auto numMessages = FlowControl::getNumMessages(msg);
    if (!hasOutboundCapacity(msg) && numMessages != 0)
    {
        CLOG_DEBUG(Overlay, "Got outbound message capacity for peer {}",
                   mConfig.toShortString(mNodeID));
    }
    mOutboundCapacity += numMessages;
}

bool
FlowControlMessageCapacity::canRead() const
{
    ZoneScoped;
    releaseAssert(mCapacity.mTotalCapacity);
    return *mCapacity.mTotalCapacity > 0;
}

FlowControlByteCapacity::FlowControlByteCapacity(Config const& cfg,
                                                 NodeID const& nodeID,
                                                 uint32_t capacity)
    : FlowControlCapacity(cfg, nodeID), mCapacityLimits{capacity, std::nullopt}
{
    mCapacity = mCapacityLimits;
}

FlowControlCapacity::ReadingCapacity
FlowControlByteCapacity::getCapacityLimits() const
{
    return mCapacityLimits;
}

uint64_t
FlowControlByteCapacity::getMsgResourceCount(StellarMessage const& msg) const
{
    return msgBodySize(msg);
}

void
FlowControlByteCapacity::releaseOutboundCapacity(StellarMessage const& msg)
{
    ZoneScoped;
    releaseAssert(msg.type() == SEND_MORE_EXTENDED);
    if (!hasOutboundCapacity(msg) &&
        (msg.sendMoreExtendedMessage().numBytes != 0))
    {
        CLOG_DEBUG(Overlay, "Got outbound byte capacity for peer {}",
                   mConfig.toShortString(mNodeID));
    }
    mOutboundCapacity += msg.sendMoreExtendedMessage().numBytes;
};

bool
FlowControlByteCapacity::canRead() const
{
    releaseAssert(!mCapacity.mTotalCapacity);
    return true;
}

void
FlowControlByteCapacity::handleTxSizeIncrease(uint32_t increase)
{
    mCapacity.mFloodCapacity += increase;
    mCapacityLimits.mFloodCapacity += increase;
}

FlowControlCapacity::FlowControlCapacity(Config const& cfg,
                                         NodeID const& nodeID)
    : mConfig(cfg), mNodeID(nodeID)
{
    releaseAssert(threadIsMain());
}

void
FlowControlCapacity::checkCapacityInvariants() const
{
    ZoneScoped;
    releaseAssert(getCapacityLimits().mFloodCapacity >=
                  mCapacity.mFloodCapacity);
    if (getCapacityLimits().mTotalCapacity)
    {
        releaseAssert(mCapacity.mTotalCapacity);
        releaseAssert(*getCapacityLimits().mTotalCapacity >=
                      *mCapacity.mTotalCapacity);
    }
    else
    {
        releaseAssert(!mCapacity.mTotalCapacity);
    }
}

void
FlowControlCapacity::lockOutboundCapacity(StellarMessage const& msg)
{
    ZoneScoped;
    if (OverlayManager::isFloodMessage(msg))
    {
        releaseAssert(hasOutboundCapacity(msg));
        mOutboundCapacity -= getMsgResourceCount(msg);
    }
}

bool
FlowControlCapacity::lockLocalCapacity(StellarMessage const& msg)
{
    ZoneScoped;
    checkCapacityInvariants();
    auto msgResources = getMsgResourceCount(msg);
    if (mCapacity.mTotalCapacity)
    {
        releaseAssert(*mCapacity.mTotalCapacity >= msgResources);
        *mCapacity.mTotalCapacity -= msgResources;
    }

    if (OverlayManager::isFloodMessage(msg))
    {
        // No capacity to process flood message
        if (mCapacity.mFloodCapacity < msgResources)
        {
            return false;
        }

        mCapacity.mFloodCapacity -= msgResources;
        if (mCapacity.mFloodCapacity == 0)
        {
            CLOG_DEBUG(Overlay, "No flood capacity for peer {}",
                       mConfig.toShortString(mNodeID));
        }
    }

    return true;
}

uint64_t
FlowControlCapacity::releaseLocalCapacity(StellarMessage const& msg)
{
    ZoneScoped;

    uint64_t releasedFloodCapacity = 0;
    size_t resourcesFreed = getMsgResourceCount(msg);
    if (mCapacity.mTotalCapacity)
    {
        *mCapacity.mTotalCapacity += resourcesFreed;
    }

    if (OverlayManager::isFloodMessage(msg))
    {
        if (mCapacity.mFloodCapacity == 0)
        {
            CLOG_DEBUG(Overlay, "Got flood capacity for peer {} ({})",
                       mConfig.toShortString(mNodeID),
                       mCapacity.mFloodCapacity + resourcesFreed);
        }
        releasedFloodCapacity = resourcesFreed;
        mCapacity.mFloodCapacity += resourcesFreed;
    }
    checkCapacityInvariants();
    return releasedFloodCapacity;
}

bool
FlowControlCapacity::hasOutboundCapacity(StellarMessage const& msg) const
{
    ZoneScoped;
    return mOutboundCapacity >= getMsgResourceCount(msg);
}

uint64_t
FlowControlCapacity::msgBodySize(StellarMessage const& msg)
{
    ZoneScoped;
    return static_cast<uint64_t>(xdr::xdr_size(msg));
}

}