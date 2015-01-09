// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "Node.h"

#include <cassert>
#include "xdrpp/marshal.h"
#include "util/Logging.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "fba/Slot.h"


namespace stellar
{

Node::Node(const uint256& nodeID,
           FBA* FBA,
           int cacheCapacity)
    : mNodeID(nodeID)
    , mFBA(FBA)
    , mCacheCapacity(cacheCapacity)
{
}

const FBAQuorumSet& 
Node::retrieveQuorumSet(const uint256& qSetHash)
{
    LOG(INFO) << "Node::retrieveQuorumSet"
              << "@" << binToHex(mNodeID).substr(0,6)
              << " "  << binToHex(qSetHash).substr(0,6);

    assert(mCacheLRU.size() == mCache.size());
    auto it = mCache.find(qSetHash);
    if (it != mCache.end()) 
    {
        return it->second;
    }
    throw QuorumSetNotFound(mNodeID, qSetHash);
}

void 
Node::addPendingSlot(const uint256& qSetHash, const uint32& slotIndex)
{
    LOG(INFO) << "Node::addPendingSlot"
              << "@" << binToHex(mNodeID).substr(0,6)
              << " " << binToHex(qSetHash).substr(0,6)
              << " " << slotIndex;
    auto it = std::find(mPendingSlots[qSetHash].begin(), 
                        mPendingSlots[qSetHash].end(),
                        slotIndex);
    if(it == mPendingSlots[qSetHash].end())
    {
        mPendingSlots[qSetHash].push_back(slotIndex);
    }
}

void
Node::cacheQuorumSet(const FBAQuorumSet& qSet)
{
    uint256 qSetHash = sha512_256(xdr::xdr_to_msg(qSet));

    LOG(INFO) << "Node::cacheQuorumSet"
              << "@" << binToHex(mNodeID).substr(0,6)
              << " "  << binToHex(qSetHash).substr(0,6);

    if (mCache.find(qSetHash) != mCache.end())
    {
        return;
    }

    while (mCacheCapacity >= 0 && 
           mCache.size() >= mCacheCapacity) 
    {
        assert(mCacheLRU.size() == mCache.size());
        auto it = mCacheLRU.begin();
        mCache.erase(*it);
        mCacheLRU.erase(it);
    }
    mCacheLRU.push_back(qSetHash);
    mCache[qSetHash] = qSet;

    for (uint32 sIndex : mPendingSlots[qSetHash])
    {
        mFBA->getSlot(sIndex)->advanceSlot();
    }
    mPendingSlots.erase(qSetHash);
}

const uint256&
Node::getNodeID()
{
    return mNodeID;
}

}
