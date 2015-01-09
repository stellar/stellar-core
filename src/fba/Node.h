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
         FBA* FBA,
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
                          const Hash& qSetHash)
            : mNodeID(nodeID)
            , mQSetHash(qSetHash) {}

        virtual const char* what() const throw()
        {
            return "QuorumSet not found";
        }
        const Hash& qSetHash() const throw() { return mQSetHash; }
        const uint256& nodeID() const throw() { return mNodeID; }

        const uint256 mNodeID;
        const Hash mQSetHash;
    };

    // Retrieves the cached quorum set associated with this hash or throws a
    // QuorumSetNotFound exception otherwise. The exception shall not escape
    // the FBA module
    const FBAQuorumSet& retrieveQuorumSet(const Hash& qSetHash);

    // Adds a slot to the list of slots awaiting for a quorunm set to be
    // retrieved. When cacheQuorumSet is called with the specified quorum set
    // hash, all slots pending on this quorum set are woken up for
    // re-evaluation.
    void addPendingSlot(const Hash& qSetHash, const uint32& slotIndex);

    void cacheQuorumSet(const FBAQuorumSet& qSet);

    const uint256& getNodeID();

  protected:
    const uint256                          mNodeID;
    FBA*                                   mFBA;
  private:
    int                                    mCacheCapacity;
    std::map<Hash, FBAQuorumSet>           mCache;
    std::vector<Hash>                      mCacheLRU;

    std::map<Hash, std::vector<uint32>>    mPendingSlots;
};
}

#endif
