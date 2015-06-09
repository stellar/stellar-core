#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <memory>
#include <vector>

#include "scp/SCP.h"
#include "util/HashOfHash.h"

namespace stellar
{
/**
 * This is one Node in the stellar network
 */
class Node
{
  protected:
    // returns a quorum set {{ nodeID }}
    static SCPQuorumSet buildSingletonQSet(uint256 const& nodeID);

    const uint256 mNodeID;
    static Hash gSingleQSetHash; // hash of the singleton qset (magic number)
    std::shared_ptr<SCPQuorumSet> mSingleQSet; // {{mNodeID}}
    SCP* mSCP;

    // called recursively
    bool isQuorumSliceInternal(SCPQuorumSet const& qset,
                               std::vector<uint256> const& nodeSet);
    bool isVBlockingInternal(SCPQuorumSet const& qset,
                             std::vector<uint256> const& nodeSet);

  public:
    static int const CACHE_SIZE;
    Node(uint256 const& nodeID, SCP* SCP);

    // returns the quorum set {{X}}
    static SCPQuorumSetPtr getSingletonQSet(uint256 const& nodeID);

    // Tests this node against nodeSet for the specified qSethash. Triggers the
    // retrieval of qSetHash for this node and may throw a QuorumSlicesNotFound
    // exception
    bool isQuorumSlice(SCPQuorumSet const& qSet,
                       std::vector<uint256> const& nodeSet);
    bool isVBlocking(SCPQuorumSet const& qSet,
                     std::vector<uint256> const& nodeSet);

    // Tests this node against a map of nodeID -> T for the specified qSetHash.
    // Triggers the retrieval of qSetHash for this node and may throw a
    // QuorumSlicesNotFound exception.

    // `isVBlocking` tests if the filtered nodes V are a v-blocking set for
    // this node.
    template <class T>
    bool
    isVBlocking(SCPQuorumSet const& qSet, std::map<uint256, T> const& map,
                std::function<bool(uint256 const&, T const&)> const& filter =
                    [](uint256 const&, T const&)
                {
                    return true;
                });
    // `isQuorum` tests if the filtered nodes V form a quorum
    // (meaning for each v \in V there is q \in Q(v)
    // included in V and we have quorum on V for qSetHash). `qfun` extracts the
    // SCPQuorumSetPtr from the template T for its associated node in map
    // (required for transitivity)
    template <class T>
    bool isQuorum(SCPQuorumSet const& qSet, std::map<uint256, T> const& map,
                  std::function<SCPQuorumSetPtr(T const&)> const& qfun,
                  std::function<bool(uint256 const&, T const&)> const& filter =
                      [](uint256 const&, T const&)
                  {
                      return true;
                  });

    /**
     * Exception used to trigger the retrieval of a quorum set based on its
     * hash when it was not cached yet. This exception should not escape the
     * SCP module.
     */
    class QuorumSlicesNotFound : public std::exception
    {
      public:
        QuorumSlicesNotFound(uint256 const& nodeID, Hash const& qSetHash)
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
    // QuorumSlicesNotFound exception otherwise. The exception shall not escape
    // the SCP module
    SCPQuorumSetPtr retrieveQuorumSet(Hash const& qSetHash);

    uint256 const& getNodeID();
};
}
