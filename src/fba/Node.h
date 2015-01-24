#pragma once

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

    // Tests this node against nodeSet for the specified qSethash. Triggers the
    // retrieval of qSetHash for this node and may throw a QuorumSetNotFound
    // exception
    bool hasQuorum(const Hash& qSetHash,
                   const std::vector<uint256>& nodeSet);
    bool isVBlocking(const Hash& qSetHash,
                     const std::vector<uint256>& nodeSet);

    // Tests this node against a map of nodeID -> T for the specified qSetHash.
    // Triggers the retrieval of qSetHash for this node and may throw a
    // QuorumSetNotFound exception.
    
    // `isVBlocking` tests if the filtered nodes V are a v-blocking set for
    // this node.
    template <class T> bool isVBlocking(
        const Hash& qSetHash,
        const std::map<uint256, T>& map,
        std::function<bool(const uint256&, const T&)> const& filter =
        [] (const uint256&, const T&) { return true; });
    // `isQuorumTransitive` tests if the filtered nodes V are a transitive
    // quorum for this node (meaning for each v \in V there is q \in Q(v)
    // included in V and we have quorum on V for qSetHash). `qfun` extracts the
    // qSetHash from the template T for its associated node in map (required
    // for transitivity)
    template <class T> bool isQuorumTransitive(
        const Hash& qSetHash,
        const std::map<uint256, T>& map,
        std::function<Hash(const T&)> const& qfun,
        std::function<bool(const uint256&, const T&)> const& filter =
        [] (const uint256&, const T&) { return true; });

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
        const uint256& nodeID() const throw() { return mNodeID; }
        const Hash& qSetHash() const throw() { return mQSetHash; }

        const uint256 mNodeID;
        const Hash mQSetHash;
    };

    // Retrieves the cached quorum set associated with this hash or throws a
    // QuorumSetNotFound exception otherwise. The exception shall not escape
    // the FBA module
    const FBAQuorumSet& retrieveQuorumSet(const Hash& qSetHash);

    // Cache a quorumSet for this node.
    void cacheQuorumSet(const FBAQuorumSet& qSet);

    const uint256& getNodeID();

  protected:
    const uint256                          mNodeID;
    FBA*                                   mFBA;

  private:
    int                                    mCacheCapacity;
    std::map<Hash, FBAQuorumSet>           mCache;
    std::vector<Hash>                      mCacheLRU;
};
}
