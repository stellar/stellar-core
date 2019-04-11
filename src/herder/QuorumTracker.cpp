// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/QuorumTracker.h"
#include "scp/LocalNode.h"

namespace stellar
{
QuorumTracker::QuorumTracker(SCP& scp) : mSCP(scp)
{
}

bool
QuorumTracker::isNodeDefinitelyInQuorum(NodeID const& id)
{
    auto it = mQuorum.find(id);
    return it != mQuorum.end();
}

bool
QuorumTracker::expand(NodeID const& id, SCPQuorumSetPtr qSet)
{
    bool res = false;
    auto it = mQuorum.find(id);
    if (it != mQuorum.end())
    {
        if (it->second == nullptr)
        {
            it->second = qSet;
            LocalNode::forAllNodes(*qSet, [&](NodeID const& id) {
                // inserts an edge node if needed
                mQuorum.insert(std::make_pair(id, nullptr));
            });
            res = true;
        }
        else if (it->second == qSet)
        {
            // nop
            res = true;
        }
    }
    return res;
}

void
QuorumTracker::rebuild(std::function<SCPQuorumSetPtr(NodeID const&)> lookup)
{
    mQuorum.clear();
    auto local = mSCP.getLocalNode();
    std::set<NodeID> backlog;
    backlog.insert(local->getNodeID());
    while (!backlog.empty())
    {
        auto n = *backlog.begin();
        backlog.erase(backlog.begin());

        auto it = mQuorum.find(n);
        if (it == mQuorum.end() || it->second == nullptr)
        {
            auto qSet = lookup(n);
            if (qSet != nullptr)
            {
                LocalNode::forAllNodes(
                    *qSet, [&](NodeID const& id) { backlog.insert(id); });
            }
            mQuorum[n] = qSet;
        }
    }
}

QuorumTracker::QuorumMap const&
QuorumTracker::getQuorum() const
{
    return mQuorum;
}
}
