#ifndef __FBA_NODE__
#define __FBA_NODE__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <memory>
#include <vector>

#include "fba/FBA.h"

namespace stellar
{
/**
 * This is one Node in the stellar network
 */
class Node : public enable_shared_from_this<Node>
{
  public:
    Node(const uint256& nodeID,
         unsigned cacheCapacity = 4);

    const FBAQuorumSet& retrieveQuorumSet(const uint256& qSetHash);
    void cacheQuorumSet(const uint256& qSetHash,
                        const FBAQuorumSet& qSet);
  private:
    uint256                         mNodeID;
    unsigned                        mCacheCapacity;
    std::map<uint256, FBAQuorumSet> mCache;
    std::vector<uint256>            mCacheLRU;
};
}

#endif
