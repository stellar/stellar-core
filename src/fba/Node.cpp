// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "Node.h"

namespace stellar
{

Node::Node(const uint256& nodeID,
           unsigned cacheCapacity)
    : mNodeID(nodeID)
    , mCacheCapacity(cacheCapacity)
{
  mQSetUnknown.nodeID = nodeID;
  mQSetUnknown.content.type(UNKNOWN);
  /* TODO(spolu) Ensure type is UNKNOWN */
}

const FBAQuorumSet& 
Node::retrieveQuorumSet(const uint256& qSetHash)
{
    auto it = mCache.find(qSetHash);
    if (it != mCache.end()) 
    {
        return it->second;
    }
    else 
    {
        return mQSetUnknown;
    }
}

void
Node::cacheQuorumSet(const uint256& qSetHash,
                     const FBAQuorumSet& qSet)
{
    while (mCache.size() >= mCacheCapacity) 
    {
        // TODO(spolu): ASSERT mCacheLRU.size() == mCache.size()
        auto it = mCacheLRU.begin();
        mCache.erase(*it);
        mCacheLRU.erase(it);
    }
    mCacheLRU.push_back(qSetHash);
    mCache[qSetHash] = qSet;
}

}
