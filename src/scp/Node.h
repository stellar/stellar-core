#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <memory>
#include <vector>

#include "scp/SCP.h"

namespace stellar
{
/**
 * This is one Node in the stellar network
 */
class Node
{

  public:
    Node(uint256 const& nodeID, SCP* SCP, int cacheCapacity = 4);

    // Tests this node against nodeSet for the specified qSethash. Triggers the
    // retrieval of qSetHash for this node and may throw a QuorumSetNotFound
    // exception
    bool hasQuorum(Hash const& qSetHash, std::vector<uint256> const& nodeSet);
    bool isVBlocking(Hash const& qSetHash, std::vector<uint256> const& nodeSet);

    // Tests this node against a map of nodeID -> T for the specified qSetHash.
    // Triggers the retrieval of qSetHash for this node and may throw a
    // QuorumSetNotFound exception.

    // `isVBlocking` tests if the filtered nodes V are a v-blocking set for
    // this node.
    template <class T>
    bool
    isVBlocking(Hash const& qSetHash, std::map<uint256, T> const& map,
                std::function<bool(uint256 const&, T const&)> const& filter =
                    [](uint256 const&, T const&)
                {
                    return true;
                });
    // `isQuorumTransitive` tests if the filtered nodes V are a transitive
    // quorum for this node (meaning for each v \in V there is q \in Q(v)
    // included in V and we have quorum on V for qSetHash). `qfun` extracts the
    // qSetHash from the template T for its associated node in map (required
    // for transitivity)
    template <class T>
    bool isQuorumTransitive(Hash const& qSetHash,
                            std::map<uint256, T> const& map,
                            std::function<Hash(T const&)> const& qfun,
                            std::function<bool(uint256 const&, T const&)> const&
                                filter = [](uint256 const&, T const&)
                            {
                                return true;
                            });

    /**
     * Exception used to trigger the retrieval of a quorum set based on its
     * hash when it was not cached yet. This exception should not escape the
     * SCP module.
     */
    class QuorumSetNotFound : public std::exception
    {
      public:
        QuorumSetNotFound(uint256 const& nodeID, Hash const& qSetHash)
            : mNodeID(nodeID), mQSetHash(qSetHash)
        {
        }

        virtual const char*
        what() const throw()
        {
            return "QuorumSet not found";
        }
        uint256 const&
        nodeID() const throw()
        {
            return mNodeID;
        }
        Hash const&
        qSetHash() const throw()
        {
            return mQSetHash;
        }

        const uint256 mNodeID;
        const Hash mQSetHash;
    };

    // Retrieves the cached quorum set associated with this hash or throws a
    // QuorumSetNotFound exception otherwise. The exception shall not escape
    // the SCP module
    SCPQuorumSet const& retrieveQuorumSet(Hash const& qSetHash);

    // Cache a quorumSet for this node.
    void cacheQuorumSet(SCPQuorumSet const& qSet);

    uint256 const& getNodeID();

    size_t getCachedQuorumSetCount() const;

  protected:
    const uint256 mNodeID;
    SCP* mSCP;

  private:
    int mCacheCapacity;
    std::map<Hash, SCPQuorumSet> mCache;
    std::vector<Hash> mCacheLRU;
};
}
