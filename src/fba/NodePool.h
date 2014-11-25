#include <map>
#include "fba/Node.h"

namespace stellar
{
    class NodePool
    {
        std::map<stellarxdr::uint256,Node::pointer> mNodeMap;
    public:
        Node::pointer getNode(stellarxdr::uint256& nodeID);
    };

    extern NodePool gNodePool;
}
