// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "SCP.h"

#include <algorithm>

#include "xdrpp/marshal.h"
#include "crypto/SHA.h"
#include "scp/LocalNode.h"
#include "scp/Slot.h"

namespace stellar
{

SCP::SCP(const SecretKey& secretKey, const SCPQuorumSet& qSetLocal)
{
    mLocalNode = std::make_shared<LocalNode>(secretKey, qSetLocal, this);
    mKnownNodes[mLocalNode->getNodeID()] = mLocalNode;
}


void
SCP::receiveEnvelope(const SCPEnvelope& envelope,
                     std::function<void(EnvelopeState)> const& cb)
{
    // If the envelope is not correctly signed, we ignore it.
    if (!verifyEnvelope(envelope))
    {
        return cb(SCP::EnvelopeState::INVALID);
    }

    uint64 slotIndex = envelope.statement.slotIndex;
    getSlot(slotIndex)->processEnvelope(envelope, cb);
}

bool
SCP::prepareValue(const uint64& slotIndex, const Value& value, bool forceBump)
{
    assert(!getSecretKey().isZero());
    return getSlot(slotIndex)->prepareValue(value, forceBump);
}

void
SCP::updateLocalQuorumSet(const SCPQuorumSet& qSet)
{
    mLocalNode->updateQuorumSet(qSet);
}

const SCPQuorumSet&
SCP::getLocalQuorumSet()
{
    return mLocalNode->getQuorumSet();
}

const uint256&
SCP::getLocalNodeID()
{
    return mLocalNode->getNodeID();
}

void
SCP::purgeNode(const uint256& nodeID)
{
    auto it = mKnownNodes.find(nodeID);
    if (it != mKnownNodes.end())
    {
        mKnownNodes.erase(it);
    }
}

void
SCP::purgeSlots(const uint64& maxSlotIndex)
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

std::shared_ptr<Node>
SCP::getNode(const uint256& nodeID)
{
    auto it = mKnownNodes.find(nodeID);
    if (it == mKnownNodes.end())
    {
        mKnownNodes[nodeID] = std::make_shared<Node>(nodeID, this);
    }
    return mKnownNodes[nodeID];
}

std::shared_ptr<LocalNode>
SCP::getLocalNode()
{
    return mLocalNode;
}

std::shared_ptr<Slot>
SCP::getSlot(const uint64& slotIndex)
{
    auto it = mKnownSlots.find(slotIndex);
    if (it == mKnownSlots.end())
    {
        mKnownSlots[slotIndex] = std::make_shared<Slot>(slotIndex, this);
    }
    return mKnownSlots[slotIndex];
}

void
SCP::signEnvelope(SCPEnvelope& envelope)
{
    assert(envelope.nodeID == getSecretKey().getPublicKey());
    envelope.signature =
        getSecretKey().sign(xdr::xdr_to_opaque(envelope.statement));
    envelopeSigned();
}

bool
SCP::verifyEnvelope(const SCPEnvelope& envelope)
{
    bool b = PublicKey::verifySig(envelope.nodeID, envelope.signature,
                                  xdr::xdr_to_opaque(envelope.statement));
    envelopeVerified(b);
    return b;
}

const SecretKey&
SCP::getSecretKey()
{
    return mLocalNode->getSecretKey();
}

bool
SCP::isVBlocking(const std::vector<uint256>& nodes)
{
    std::map<uint256, bool> map;
    for (auto v : nodes)
    {
        map[v] = true;
    }
    return getLocalNode()->isVBlocking<bool>(getLocalNode()->getQuorumSetHash(),
                                             map);
}

size_t
SCP::getKnownNodesCount() const
{
    return mKnownNodes.size();
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

size_t
SCP::getCumulativeCachedQuorumSetCount() const
{
    size_t c = 0;
    for (auto const& n : mKnownNodes)
    {
        c += n.second->getCachedQuorumSetCount();
    }
    return c;
}
}
