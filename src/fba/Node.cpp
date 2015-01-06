// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "Node.h"

#include <cassert>
#include "xdrpp/marshal.h"
#include "crypto/SHA.h"

namespace stellar
{

Node::Node(const uint256& nodeID,
           int cacheCapacity)
    : mNodeID(nodeID)
    , mCacheCapacity(cacheCapacity)
{
}

const FBAQuorumSet& 
Node::retrieveQuorumSet(const uint256& qSetHash)
{
    assert(mCacheLRU.size() == mCache.size());
    auto it = mCache.find(qSetHash);
    if (it != mCache.end()) 
    {
        return it->second;
    }
    throw QuorumSetNotFound(mNodeID, qSetHash);
}

void
Node::cacheQuorumSet(const FBAQuorumSet& qSet)
{
    uint256 qSetHash = sha512_256(xdr::xdr_to_msg(qSet));

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
}

const uint256&
Node::getNodeID()
{
  return mNodeID;
}

}
