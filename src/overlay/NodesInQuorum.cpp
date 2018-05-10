// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/NodesInQuorum.h"
#include "main/Application.h"
#include "scp/SCP.h"

constexpr const int NODES_QUORUM_CACHE_SIZE = 1000;

namespace stellar
{

NodesInQuorum::NodesInQuorum(Application& app)
    : mApp(app), mNodesInQuorum(NODES_QUORUM_CACHE_SIZE)
{
}

bool
NodesInQuorum::isNodeInQuorum(NodeID const& node)
{
    bool res;

    res = mNodesInQuorum.exists(node);
    if (res)
    {
        res = mNodesInQuorum.get(node);
    }
    else
    {
        // search through the known slots
        SCP::TriBool r = mApp.getHerder().getSCP().isNodeInQuorum(node);

        // consider a node in quorum if it's either in quorum
        // or we don't know if it is (until we get further evidence)
        res = (r != SCP::TB_FALSE);

        mNodesInQuorum.put(node, res);
    }

    return res;
}

void
NodesInQuorum::clear()
{
    mNodesInQuorum.clear();
}
}
