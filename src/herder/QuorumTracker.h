#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "scp/SCP.h"
#include "util/NonCopyable.h"
#include "util/UnorderedMap.h"
#include "util/UnorderedSet.h"
#include <deque>
#include <set>

namespace stellar
{

// helper class to help track the overall quorum over time
// a node tracked is definitely in the transitive quorum.
// If its associated quorum set is empty (nullptr), it just means
// that another node has that node in its quorum set
// but could not explore the quorum further (as we're missing the quorum set)
// Nodes can be added one by one (calling `expand`, most efficient)
// or the quorum can be rebuilt from scratch by using a lookup function
class QuorumTracker : public NonMovableOrCopyable
{
  public:
    struct NodeInfo
    {
        SCPQuorumSetPtr mQuorumSet;

        // The next two fields represent distance to the local node and a set of
        // validators in the local qset that are closest to the node that
        // NodeInfo represents. If NodeInfo is the local node, mDistance is 0
        // and mClosestValidators is empty. If NodeInfo is a node in the local
        // qset, mDistance is 1 and mClosestValidators only contains the local
        // qset node. Otherwise, mDistance is the shortest distance to NodeInfo
        // from the local node, and mClosestValidators contains all validators
        // in the local qset that are (mDistance - 1) away from NodeInfo.
        int mDistance;
        std::set<NodeID> mClosestValidators;
    };

    using QuorumMap = UnorderedMap<NodeID, NodeInfo>;

  private:
    NodeID const mLocalNodeID;
    QuorumMap mQuorum;

  public:
    QuorumTracker(NodeID const& localNodeID);

    // returns true if id is in transitive quorum for sure
    bool isNodeDefinitelyInQuorum(NodeID const& id);

    // attempts to expand quorum at node `id`
    // expansion here means adding `id` to the known quorum
    // and add its dependencies as defined by `qset`
    // returns true if expansion succeeded
    //     `id` was known and had a qset identical to the one we try to fill in
    //     `id` was known and didn't have a quorumset
    // returns false on failure
    //     `id` was unknown
    //     `id` was known and had a quorum set different from the one we try to
    //     fill in
    // if expand fails, the caller should instead use `rebuild`
    //
    // `expand` additionally populates the closest validators set in the
    // NodeInfo for id. For every node outside of the local qset, keep track of
    // the nodes in the qset, which are equally close to the external node
    bool expand(NodeID const& id, SCPQuorumSetPtr qSet);

    // rebuild the transitive quorum given a lookup function
    void rebuild(std::function<SCPQuorumSetPtr(NodeID const&)> lookup);

    // returns the current known quorum
    QuorumMap const& getQuorum() const;

    // Given a node in the transitive quorum, but _outside_ of the local quorum
    // set, find all nodes in the local quorum set with the shortest distance to
    // `nodeOutsideQset`. If a node from the local quorum set is passed in,
    // return itself. If the local node is passed in, return an empty set.
    std::set<NodeID> const&
    findClosestValidators(NodeID const& nodeOutsideQset);
};
}
