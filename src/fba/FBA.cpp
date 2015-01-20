// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "FBA.h"

#include "fba/LocalNode.h"
#include "fba/Slot.h"

namespace stellar
{

FBA::FBA(const uint256& validationSeed,
         const FBAQuorumSet& qSetLocal)
{
    mLocalNode = new LocalNode(validationSeed, qSetLocal, this);
    mKnownNodes[mLocalNode->getNodeID()] = mLocalNode;
}

FBA::~FBA()
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
FBA::receiveEnvelope(const FBAEnvelope& envelope,
                     std::function<void(EnvelopeState)> const& cb)
{
    // If the envelope is not correctly signed, we ignore it.
    if (!verifyEnvelope(envelope))
    {
        return cb(FBA::EnvelopeState::INVALID);
    }

    uint64 slotIndex = envelope.slotIndex;
    getSlot(slotIndex)->processEnvelope(envelope, cb);
}

bool
FBA::prepareValue(const uint64& slotIndex,
                  const Value& value,
                  bool forceBump)
{
    return getSlot(slotIndex)->prepareValue(value, forceBump);
}

void 
FBA::updateLocalQuorumSet(const FBAQuorumSet& qSet)
{
    mLocalNode->updateQuorumSet(qSet);
}

const FBAQuorumSet& 
FBA::getLocalQuorumSet()
{
    return mLocalNode->getQuorumSet();
}

const uint256& 
FBA::getLocalNodeID()
{
  return mLocalNode->getNodeID();
}

Node* 
FBA::getNode(const uint256& nodeID)
{
    auto it = mKnownNodes.find(nodeID);
    if (it == mKnownNodes.end())
    {
        mKnownNodes[nodeID] = new Node(nodeID, this);
    }
    return mKnownNodes[nodeID];
}

LocalNode* 
FBA::getLocalNode()
{
  return mLocalNode;
}

Slot*
FBA::getSlot(const uint64& slotIndex)
{
    auto it = mKnownSlots.find(slotIndex);
    if (it == mKnownSlots.end())
    {
        mKnownSlots[slotIndex] = new Slot(slotIndex, this);
    }
    return mKnownSlots[slotIndex];
}

void
FBA::signEnvelope(FBAEnvelope& envelope)
{
    // TODO(spolu) envelope signature
}

bool 
FBA::verifyEnvelope(const FBAEnvelope& envelope)
{
    // TODO(spolu) envelope verification
    return true;
}


}
