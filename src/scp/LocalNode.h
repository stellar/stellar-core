#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <memory>
#include <set>
#include <vector>

#include "scp/SCP.h"
#include "util/HashOfHash.h"

namespace stellar
{
/**
 * This is one Node in the stellar network
 */
class LocalNode
{
  protected:
    const NodeID mNodeID;
    const bool mIsValidator;
    SCPQuorumSet mQSet;
    Hash mQSetHash;

    // alternative qset used during externalize {{mNodeID}}
    Hash gSingleQSetHash;                      // hash of the singleton qset
    std::shared_ptr<SCPQuorumSet> mSingleQSet; // {{mNodeID}}

    SCP* mSCP;

  public:
    LocalNode(NodeID const& nodeID, bool isValidator, SCPQuorumSet const& qSet,
              SCP* scp);

    NodeID const& getNodeID();

    void updateQuorumSet(SCPQuorumSet const& qSet);

    SCPQuorumSet const& getQuorumSet();
    Hash const& getQuorumSetHash();
    bool isValidator();

    // returns the quorum set {{X}}
    static SCPQuorumSetPtr getSingletonQSet(NodeID const& nodeID);

    // runs proc over all nodes contained in qset, but fast fails if proc fails
    static bool forAllNodes(SCPQuorumSet const& qset,
                            std::function<bool(NodeID const&)> proc);

    // returns the weight of the node within the qset
    // normalized between 0-UINT64_MAX
    static uint64 getNodeWeight(NodeID const& nodeID, SCPQuorumSet const& qset);

    // Tests this node against nodeSet for the specified qSethash.
    static bool isQuorumSlice(SCPQuorumSet const& qSet,
                              std::vector<NodeID> const& nodeSet);
    static bool isVBlocking(SCPQuorumSet const& qSet,
                            std::vector<NodeID> const& nodeSet);

    // Tests this node against a map of nodeID -> T for the specified qSetHash.

    // `isVBlocking` tests if the filtered nodes V are a v-blocking set for
    // this node.
    static bool isVBlocking(
        SCPQuorumSet const& qSet,
        std::map<NodeID, SCPEnvelopeWrapperPtr> const& map,
        std::function<bool(SCPStatement const&)> const& filter =
            [](SCPStatement const&) { return true; });

    // `isQuorum` tests if the filtered nodes V form a quorum
    // (meaning for each v \in V there is q \in Q(v)
    // included in V and we have quorum on V for qSetHash). `qfun` extracts the
    // SCPQuorumSetPtr from the SCPStatement for its associated node in map
    // (required for transitivity)
    static bool isQuorum(
        SCPQuorumSet const& qSet,
        std::map<NodeID, SCPEnvelopeWrapperPtr> const& map,
        std::function<SCPQuorumSetPtr(SCPStatement const&)> const& qfun,
        std::function<bool(SCPStatement const&)> const& filter =
            [](SCPStatement const&) { return true; });

    // computes the distance to the set of v-blocking sets given
    // a set of nodes that agree (but can fail)
    // excluded, if set will be skipped altogether
    static std::vector<NodeID>
    findClosestVBlocking(SCPQuorumSet const& qset,
                         std::set<NodeID> const& nodes, NodeID const* excluded);

    static std::vector<NodeID> findClosestVBlocking(
        SCPQuorumSet const& qset,
        std::map<NodeID, SCPEnvelopeWrapperPtr> const& map,
        std::function<bool(SCPStatement const&)> const& filter =
            [](SCPStatement const&) { return true; },
        NodeID const* excluded = nullptr);

    static Json::Value toJson(SCPQuorumSet const& qSet,
                              std::function<std::string(PublicKey const&)> r);

    Json::Value toJson(SCPQuorumSet const& qSet, bool fullKeys) const;
    std::string to_string(SCPQuorumSet const& qSet) const;

    static uint64 computeWeight(uint64 m, uint64 total, uint64 threshold);

  protected:
    // returns a quorum set {{ nodeID }}
    static SCPQuorumSet buildSingletonQSet(NodeID const& nodeID);

    // called recursively
    static bool isQuorumSliceInternal(SCPQuorumSet const& qset,
                                      std::vector<NodeID> const& nodeSet);
    static bool isVBlockingInternal(SCPQuorumSet const& qset,
                                    std::vector<NodeID> const& nodeSet);
};
}
