// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "SCP.h"

#include <algorithm>

#include "xdrpp/marshal.h"
#include "crypto/SHA.h"
#include "scp/LocalNode.h"
#include "scp/Slot.h"
#include "util/Logging.h"
#include "crypto/Hex.h"
#include "util/GlobalChecks.h"

namespace stellar
{
using xdr::operator==;

SCP::SCP(SCPDriver& driver, SecretKey const& secretKey, bool isValidator,
         SCPQuorumSet const& qSetLocal)
    : mDriver(driver)
{
    mLocalNode =
        std::make_shared<LocalNode>(secretKey, isValidator, qSetLocal, this);
}

SCP::EnvelopeState
SCP::receiveEnvelope(SCPEnvelope const& envelope)
{
    // If the envelope is not correctly signed, we ignore it.
    if (!mDriver.verifyEnvelope(envelope))
    {
        CLOG(DEBUG, "SCP") << "SCP::receiveEnvelope invalid";
        return SCP::EnvelopeState::INVALID;
    }

    uint64 slotIndex = envelope.statement.slotIndex;
    return getSlot(slotIndex, true)->processEnvelope(envelope);
}

bool
SCP::abandonBallot(uint64 slotIndex)
{
    dbgAssert(isValidator());
    return getSlot(slotIndex, true)->abandonBallot();
}

bool
SCP::nominate(uint64 slotIndex, Value const& value, Value const& previousValue)
{
    dbgAssert(isValidator());
    return getSlot(slotIndex, true)->nominate(value, previousValue, false);
}

void
SCP::stopNomination(uint64 slotIndex)
{
    auto s = getSlot(slotIndex, false);
    if (s)
    {
        s->stopNomination();
    }
}

void
SCP::updateLocalQuorumSet(SCPQuorumSet const& qSet)
{
    mLocalNode->updateQuorumSet(qSet);
}

SCPQuorumSet const&
SCP::getLocalQuorumSet()
{
    return mLocalNode->getQuorumSet();
}

NodeID const&
SCP::getLocalNodeID()
{
    return mLocalNode->getNodeID();
}

void
SCP::purgeSlots(uint64 maxSlotIndex)
{
    auto it = mKnownSlots.begin();
    while (it != mKnownSlots.end())
    {
        if (it->first < maxSlotIndex)
        {
            it = mKnownSlots.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

std::shared_ptr<LocalNode>
SCP::getLocalNode()
{
    return mLocalNode;
}

std::shared_ptr<Slot>
SCP::getSlot(uint64 slotIndex, bool create)
{
    std::shared_ptr<Slot> res;
    auto it = mKnownSlots.find(slotIndex);
    if (it == mKnownSlots.end())
    {
        if (create)
        {
            res = std::make_shared<Slot>(slotIndex, *this);
            mKnownSlots[slotIndex] = res;
        }
    }
    else
    {
        res = it->second;
    }
    return res;
}

void
SCP::dumpInfo(Json::Value& ret, size_t limit)
{
    auto it = mKnownSlots.rbegin();
    while (it != mKnownSlots.rend() && limit-- != 0)
    {
        it->second->dumpInfo(ret);
        it++;
    }
}

void
SCP::dumpQuorumInfo(Json::Value& ret, NodeID const& id, bool summary,
                    uint64 index)
{
    if (index == 0)
    {
        for (auto& item : mKnownSlots)
        {
            item.second->dumpQuorumInfo(ret, id, summary);
        }
    }
    else
    {
        auto s = getSlot(index, false);
        if (s)
        {
            s->dumpQuorumInfo(ret, id, summary);
        }
    }
}

SecretKey const&
SCP::getSecretKey()
{
    return mLocalNode->getSecretKey();
}

bool
SCP::isValidator()
{
    return mLocalNode->isValidator();
}

size_t
SCP::getKnownSlotsCount() const
{
    return mKnownSlots.size();
}

size_t
SCP::getCumulativeStatemtCount() const
{
    size_t c = 0;
    for (auto const& s : mKnownSlots)
    {
        c += s.second->getStatementCount();
    }
    return c;
}

std::vector<SCPEnvelope>
SCP::getLatestMessagesSend(uint64 slotIndex)
{
    auto slot = getSlot(slotIndex, false);
    if (slot)
    {
        return slot->getLatestMessagesSend();
    }
    else
    {
        return std::vector<SCPEnvelope>();
    }
}

void
SCP::setStateFromEnvelope(uint64 slotIndex, SCPEnvelope const& e)
{
    if (mDriver.verifyEnvelope(e))
    {
        auto slot = getSlot(slotIndex, true);
        slot->setStateFromEnvelope(e);
    }
}

std::vector<SCPEnvelope>
SCP::getCurrentState(uint64 slotIndex)
{
    auto slot = getSlot(slotIndex, false);
    if (slot)
    {
        return slot->getCurrentState();
    }
    else
    {
        return std::vector<SCPEnvelope>();
    }
}
}
