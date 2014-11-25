#include "fba/NodePool.h"
#include "lib/util/types.h"

namespace stellar
{
    NodePool gNodePool;

    Node::pointer NodePool::getNode(stellarxdr::uint256& nodeID)
    {
        Node::pointer ret = mNodeMap[nodeID];
        if(isZero(ret->mNodeID))
        {  // we have never heard of this node
            ret->mNodeID = nodeID;
        }
        return ret;
    }
}