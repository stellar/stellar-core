// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

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
    mLocalNode = new LocalNode(secretKey, qSetLocal, this);
    mKnownNodes[mLocalNode->getNodeID()] = mLocalNode;
}

SCP::~SCP()
{
    for (auto it : mKnownNodes)
    {
        delete it.second;
    }
    for (auto it : mKnownSlots)
    {
        delete it.second;
    }
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
        delete it->second;
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
            delete it->second;
            it = mKnownSlots.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

Node*
SCP::getNode(const uint256& nodeID)
{
    auto it = mKnownNodes.find(nodeID);
    if (it == mKnownNodes.end())
    {
        mKnownNodes[nodeID] = new Node(nodeID, this);
    }
    return mKnownNodes[nodeID];
}

LocalNode*
SCP::getLocalNode()
{
    return mLocalNode;
}

Slot*
SCP::getSlot(const uint64& slotIndex)
{
    auto it = mKnownSlots.find(slotIndex);
    if (it == mKnownSlots.end())
    {
        mKnownSlots[slotIndex] = new Slot(slotIndex, this);
    }
    return mKnownSlots[slotIndex];
}

void
SCP::signEnvelope(SCPEnvelope& envelope)
{
    assert(envelope.nodeID == getSecretKey().getPublicKey());
    envelope.signature =
        getSecretKey().sign(xdr::xdr_to_msg(envelope.statement));
    envelopeSigned();
}

bool
SCP::verifyEnvelope(const SCPEnvelope& envelope)
{
    bool b = PublicKey::verifySig(envelope.nodeID, envelope.signature,
                                  xdr::xdr_to_msg(envelope.statement));
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
}
