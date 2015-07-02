#include "lib/catch.hpp"
#include "scp/LocalNode.h"
#include "simulation/Simulation.h"

namespace stellar
{
bool
isNear(uint64 r, double target)
{
    double v = (double)r / (double)UINT64_MAX;
    return (std::abs(v - target) < .01);
}

TEST_CASE("nomination weight", "[scp]")
{
    SIMULATION_CREATE_NODE(0);
    SIMULATION_CREATE_NODE(1);
    SIMULATION_CREATE_NODE(2);
    SIMULATION_CREATE_NODE(3);
    SIMULATION_CREATE_NODE(4);
    SIMULATION_CREATE_NODE(5);

    SCPQuorumSet qSet;
    qSet.threshold = 3;
    qSet.validators.push_back(v0NodeID);
    qSet.validators.push_back(v1NodeID);
    qSet.validators.push_back(v2NodeID);
    qSet.validators.push_back(v3NodeID);

    uint64 result = LocalNode::getNodeWeight(v2NodeID, qSet);

    REQUIRE(isNear(result, .75));

    result = LocalNode::getNodeWeight(v4NodeID, qSet);
    REQUIRE(result == 0);

    SCPQuorumSet iQSet;
    iQSet.threshold = 1;
    iQSet.validators.push_back(v4NodeID);
    iQSet.validators.push_back(v5NodeID);
    qSet.innerSets.push_back(iQSet);

    result = LocalNode::getNodeWeight(v4NodeID, qSet);

    REQUIRE(isNear(result, .6 * .5));
}
}