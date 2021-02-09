// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/QuorumTracker.h"
#include "scp/LocalNode.h"
#include "util/GlobalChecks.h"
#include <Tracy.hpp>

namespace stellar
{
QuorumTracker::QuorumTracker(NodeID const& localNodeID)
    : mLocalNodeID(localNodeID)
{
}

bool
QuorumTracker::isNodeDefinitelyInQuorum(NodeID const& id)
{
    auto it = mQuorum.find(id);
    return it != mQuorum.end();
}

// Expand is called from two contexts. In the first, it's attempting to make
// incremental updates; a `false` return in that context means we can't do a
// successful incremental update (due to some inconsistency) and we need to
// flush and rebuild the data structure.
//
// The other context is a call from _within_ `rebuild`, which should have
// cleaned out the structure and only be rebuilding a consistent state;
// returning `false` in that context should be impossible and will be treated as
// a logic error, throwing an exception.
bool
QuorumTracker::expand(NodeID const& id, SCPQuorumSetPtr qSet)
{
    ZoneScoped;

    releaseAssertOrThrow(qSet);
    auto it = mQuorum.find(id);
    if (it == mQuorum.end())
    {
        // We only expand nodes we've heard of before, from some previous call
        // to expand that populated mQuorum with a nullptr qset for `id`
        // (starting from the base case of the local node). Thus a total
        // stranger is considered an inconsistency.
        return false;
    }

    auto& nodeInfo = it->second;
    if (nodeInfo.mQuorumSet != nullptr)
    {
        // If we _have_ heard of a node, and it in turn already _has_ a qset,
        // that means it wasn't just a leaf `qSet` member of some earlier
        // expand; it was the `id` argument of a call to expand before; we check
        // that the qset recorded in the past is the same one we were asked to
        // expand-with in the current call. If not, it's an inconsistency.
        return nodeInfo.mQuorumSet == qSet;
    }

    // Finally, we've got a node we've heard of (it was present in `mQuorum`),
    // but doesn't have an `mQuorumSet` in its `NodeInfo` yet, meaning that its
    // `NodeInfo` structure hasn't been filled in yet. We're going to try to
    // fill it in, which will also involve adding or updating (possibly leaf)
    // `NodeInfo` entries in `mQuorum` for each of `qNode` mentioned in `qSet`.

    releaseAssertOrThrow(nodeInfo.mQuorumSet == nullptr);
    nodeInfo.mQuorumSet = qSet;
    int newDist = nodeInfo.mDistance + 1;

    return LocalNode::forAllNodes(*qSet, [&](NodeID const& qNode) {
        auto qPair = mQuorum.emplace(qNode, NodeInfo{nullptr, newDist});

        bool exists = !qPair.second;
        NodeInfo& qNodeInfo = qPair.first->second;

        // `mQuorum` already contains an entry for `qNode`.
        // Check if it can be replaced with a better entry.
        if (exists)
        {
            if (qNodeInfo.mDistance < newDist)
            {
                // There's already a shorter path from the local node to
                // `qNode`, which means the "closest validators" set for
                // `qNode` is already correct and we don't need to touch
                // it.
                return true;
            }
            else if (qNodeInfo.mQuorumSet)
            {
                // In remaining cases, we're about to add to (or replace)
                // the `mClosestValidators` field of `qNodeInfo`. We
                // can only safely do this if it's still a leaf node (we've
                // not yet expanded it). If its `mQuorumSet` is non-null,
                // we've already expanded it and we'd be possibly making an
                // inconsistency by updating it after the fact.
                return false;
            }
            else if (newDist < qNodeInfo.mDistance)
            {
                // If `newDist` is a new, shorter path to `qNodeInfo`,
                // we are going to be _replacing_ its `mClosestValidators`
                // set, not just expanding the set of paths to it.
                qNodeInfo.mClosestValidators.clear();
                qNodeInfo.mDistance = newDist;
            }
            else
            {
                // The remaining case should be that we're going to
                // add a way to connect _at the same distance_ to the
                // existing node.
                releaseAssertOrThrow(newDist == qNodeInfo.mDistance);
            }
        }

        if (newDist == 1)
        {
            // The base case happens when a node is a member of the local
            // node's qset: then the `mClosestValidators` set for that node
            // contains the node itself.
            qNodeInfo.mClosestValidators.emplace(qNode);
        }
        else
        {
            // Otherwise we populate the existing `mClosestValidators` set
            // with that of the node that depends on it.
            qNodeInfo.mClosestValidators.insert(
                nodeInfo.mClosestValidators.begin(),
                nodeInfo.mClosestValidators.end());
        }
        return true;
    });
}

void
QuorumTracker::rebuild(std::function<SCPQuorumSetPtr(NodeID const&)> lookup)
{
    ZoneScoped;

    mQuorum.clear();

    mQuorum.emplace(mLocalNodeID, NodeInfo{nullptr, 0});

    // Perform a full rebuild of the transitive qset via a BFS traversal
    // Traversal by distance yields optimal shortest paths because all edge
    // weights are identical. Visited nodes are implicitly marked by mQuorumSet
    // pointer, which is set if the node is visited, and nullptr otherwise.
    std::deque<NodeID> backlog{mLocalNodeID};

    while (!backlog.empty())
    {
        auto const& node = backlog.front();

        auto it = mQuorum.find(node);
        if (it != mQuorum.end())
        {
            if (it->second.mQuorumSet == nullptr)
            {
                auto qSet = lookup(node);

                if (qSet)
                {
                    LocalNode::forAllNodes(*qSet, [&](NodeID const& id) {
                        backlog.emplace_back(id);
                        return true;
                    });

                    bool success = expand(node, qSet);
                    if (!success)
                    {
                        // As closer nodes are always processed first, `expand`
                        // must succeed
                        throw std::runtime_error("Invalid state while "
                                                 "rebuilding quorum state: "
                                                 "expand failed");
                    }
                }
            }
        }
        else
        {
            throw std::runtime_error("Invalid state while rebuilding quorum "
                                     "state: node not in quorum map");
        }
        backlog.pop_front();
    }
}

QuorumTracker::QuorumMap const&
QuorumTracker::getQuorum() const
{
    return mQuorum;
}

std::set<NodeID> const&
QuorumTracker::findClosestValidators(NodeID const& nodeOutsideQset)
{
    releaseAssertOrThrow(isNodeDefinitelyInQuorum(nodeOutsideQset));
    return mQuorum[nodeOutsideQset].mClosestValidators;
}
}