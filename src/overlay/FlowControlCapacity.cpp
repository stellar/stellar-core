// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/FlowControlCapacity.h"
#include "main/Application.h"
#include "overlay/OverlayManager.h"
#include "util/Logging.h"

namespace stellar
{

FlowControlCapacity::FlowControlCapacity(Application& app, NodeID nodeID)
    : mApp(app), mNodeID(nodeID)
{
}

void
FlowControlCapacity::checkCapacityInvariants() const
{
    auto limits = getCapacityLimits();
    releaseAssert(getCapacityLimits().mFloodCapacity >=
                  mCapacity.mFloodCapacity);
    if (limits.mTotalCapacity)
    {
        releaseAssert(mCapacity.mTotalCapacity);
        releaseAssert(*limits.mTotalCapacity >= *mCapacity.mTotalCapacity);
    }
    else
    {
        releaseAssert(!mCapacity.mTotalCapacity);
    }
}

void
FlowControlCapacity::lockOutboundCapacity(StellarMessage const& msg)
{
    if (mApp.getOverlayManager().isFloodMessage(msg))
    {
        releaseAssert(hasOutboundCapacity(msg));
        mOutboundCapacity -= getMsgResourceCount(msg);
    }
}

bool
FlowControlCapacity::lockLocalCapacity(StellarMessage const& msg)
{
    checkCapacityInvariants();
    auto msgResources = getMsgResourceCount(msg);
    if (mCapacity.mTotalCapacity)
    {
        releaseAssert(mCapacity.mTotalCapacity >= msgResources);
        *mCapacity.mTotalCapacity -= msgResources;
    }

    if (mApp.getOverlayManager().isFloodMessage(msg))
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
                       mApp.getConfig().toShortString(mNodeID));
        }
    }

    return true;
}

uint64_t
FlowControlCapacity::releaseLocalCapacity(StellarMessage const& msg)
{
    uint64_t releasedFloodCapacity = 0;
    size_t resourcesFreed = getMsgResourceCount(msg);
    if (mCapacity.mTotalCapacity)
    {
        *mCapacity.mTotalCapacity += resourcesFreed;
    }

    if (mApp.getOverlayManager().isFloodMessage(msg))
    {
        if (mCapacity.mFloodCapacity == 0)
        {
            CLOG_DEBUG(Overlay, "Got flood capacity for peer {} ({})",
                       mApp.getConfig().toShortString(mNodeID),
                       mCapacity.mFloodCapacity + resourcesFreed);
        }
        releasedFloodCapacity = resourcesFreed;
        mCapacity.mFloodCapacity += resourcesFreed;
    }
    checkCapacityInvariants();
    return releasedFloodCapacity;
}

FlowControlCapacityMessages::FlowControlCapacityMessages(Application& app,
                                                         NodeID nodeID)
    : FlowControlCapacity(app, nodeID)
{
    mCapacity = getCapacityLimits();
}

FlowControlCapacity::ReadingCapacity
FlowControlCapacityMessages::getCapacityLimits() const
{
    return {
        mApp.getConfig().PEER_FLOOD_READING_CAPACITY,
        std::make_optional<uint64_t>(mApp.getConfig().PEER_READING_CAPACITY)};
}

uint64_t
FlowControlCapacityMessages::getMsgResourceCount(StellarMessage const& msg)
{
    // Each message takes one unit of capacity
    return 1;
}

void
FlowControlCapacityMessages::releaseOutboundCapacity(StellarMessage const& msg)
{
    releaseAssert(msg.type() == SEND_MORE || msg.type() == SEND_MORE_EXTENDED);
    auto numMessages = Peer::getNumMessages(msg);
    if (!hasOutboundCapacity(msg) && numMessages != 0)
    {
        CLOG_DEBUG(Overlay, "Got outbound capacity for peer {}",
                   mApp.getConfig().toShortString(mNodeID));
    }
    mOutboundCapacity += numMessages;
}

bool
FlowControlCapacityMessages::hasOutboundCapacity(StellarMessage const& msg)
{
    return mOutboundCapacity > 0;
}

FlowControlCapacityBytes::FlowControlCapacityBytes(Application& app,
                                                   NodeID nodeID)
    : FlowControlCapacity(app, nodeID)
{
    mCapacity = getCapacityLimits();
}

FlowControlCapacity::ReadingCapacity
FlowControlCapacityBytes::getCapacityLimits() const
{
    return {mApp.getConfig().PEER_FLOOD_READING_CAPACITY_BYTES, std::nullopt};
}

uint64_t
FlowControlCapacityBytes::getMsgResourceCount(StellarMessage const& msg)
{

    return static_cast<uint64_t>(xdr::xdr_argpack_size(msg));
}

void
FlowControlCapacityBytes::releaseOutboundCapacity(StellarMessage const& msg)
{
    releaseAssert(msg.type() == SEND_MORE_EXTENDED);
    if (!hasOutboundCapacity(msg) &&
        (msg.sendMoreExtendedMessage().numBytes != 0))
    {
        CLOG_DEBUG(Overlay, "Got outbound capacity for peer {}",
                   mApp.getConfig().toShortString(mNodeID));
    }
    mOutboundCapacity += msg.sendMoreExtendedMessage().numBytes;
};

bool
FlowControlCapacityBytes::hasOutboundCapacity(StellarMessage const& msg)
{
    return mOutboundCapacity >= getMsgResourceCount(msg);
}

}