#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "scp/SCP.h"
#include "util/NonCopyable.h"
#include <unordered_map>

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
    using QuorumMap = std::unordered_map<NodeID, SCPQuorumSetPtr>;

  private:
    SCP& mSCP;
    QuorumMap mQuorum;

  public:
    QuorumTracker(SCP& scp);

    // returns true if id is in transitive quorum for sure
    bool isNodeDefinitelyInQuorum(NodeID const& id);

    // attempts to expand quorum at node `id`
    // expansion here means adding `id` to the known quorum
    // and add its dependencies as defined by `qset`
    // returns true if expansion succeeded
    //     `id` was unknown
    //     `id` was known and didn't have a quorumset
    // returns false on failure
    // if expand fails, the caller should instead use `rebuild`
    bool expand(NodeID const& id, SCPQuorumSetPtr qSet);

    // rebuild the transitive quorum given a lookup function
    void rebuild(std::function<SCPQuorumSetPtr(NodeID const&)> lookup);

    // returns the current known quorum
    QuorumMap const& getQuorum() const;
};
}
