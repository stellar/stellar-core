// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "FBA.h"

#include "fba/LocalNode.h"
#include "fba/Slot.h"

namespace stellar
{

FBA::FBA(const uint256& validationSeed,
         bool validating, 
         const FBAQuorumSet& qSetLocal,
         Client* client)
    : mValidating(validating)
    , mClient(client)
{
    mLocalNode = new LocalNode(validationSeed, qSetLocal);
    mKnownNodes[mLocalNode->getNodeID()] = mLocalNode;
}

void
FBA::receiveQuorumSet(const FBAQuorumSet& qSet)
{
    getNode(qSet.nodeID)->cacheQuorumSet(qSet);
}

void
FBA::receiveEnvelope(const FBAEnvelope& envelope)
{
    uint32 slotIndex = envelope.statement.slotIndex;
    auto it = mKnownSlots.find(slotIndex);
    if (it == mKnownSlots.end())
    {
        mKnownSlots[slotIndex] = new Slot(slotIndex, this);
    }
    mKnownSlots[slotIndex]->processEnvelope(envelope);
}

bool
FBA::attemptValue(const uint32& slotIndex,
                  const uint256& valueHash)
{
    auto it = mKnownSlots.find(slotIndex);
    if (it == mKnownSlots.end())
    {
        mKnownSlots[slotIndex] = new Slot(slotIndex, this);
    }
    return mKnownSlots[slotIndex]->attemptValue(valueHash);
}

void 
FBA::setLocalQuorumSet(const FBAQuorumSet& qSet)
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
        mKnownNodes[nodeID] = new Node(nodeID);
    }
    return mKnownNodes[nodeID];
}

LocalNode* 
FBA::getLocalNode()
{
  return mLocalNode;
}

FBA::Client*
FBA::getClient()
{
    return mClient;
}

}
