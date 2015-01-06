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
class Node
{
  public:
    Node(const uint256& nodeID,
         int cacheCapacity = 4);

    /**
     * Exception used to trigger the retrieval of a quorum set based on its
     * hash when it was not cached yet. This exception should not escape the
     * FBA module.
     */
    class QuorumSetNotFound : public std::exception
    {
      public:
        QuorumSetNotFound(const uint256& nodeID,
                          const uint256& qSetHash)
            : mNodeID(nodeID)
            , mQSetHash(qSetHash) {}

        virtual const char* what() const throw()
        {
            return "QuorumSet not found";
        }
        const uint256& qSetHash() const throw() { return mQSetHash; }
        const uint256& nodeID() const throw() { return mNodeID; }

        const uint256& mNodeID;
        const uint256& mQSetHash;
    };

    // Retrieves the cached quorum set associated with this hash or throws a
    // QuorumSetNotFound exception otherwise. The exception shall not escape
    // the FBA module
    const FBAQuorumSet& retrieveQuorumSet(const uint256& qSetHash);

    void cacheQuorumSet(const FBAQuorumSet& qSet);

    const uint256& getNodeID();

  protected:
    const uint256&                  mNodeID;
  private:
    int                             mCacheCapacity;
    std::map<uint256, FBAQuorumSet> mCache;
    std::vector<uint256>            mCacheLRU;
};
}

#endif
