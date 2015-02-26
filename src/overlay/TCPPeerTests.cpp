#include "util/Timer.h"
#include "TCPPeer.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/test.h"
#include "overlay/PeerDoor.h"
#include "main/Config.h"
#include "util/Logging.h"
#include "simulation/Simulation.h"

namespace stellar
{


TEST_CASE("TCPPeer can communicate", "[overlay]")
{
    Simulation::pointer s = make_shared<Simulation>(false);

    SIMULATION_CREATE_NODE(0);
    SIMULATION_CREATE_NODE(1);

    auto n0 = s->addNode(v0VSeed, FBAQuorumSet(), s->getClock());
    auto n1 = s->addNode(v1VSeed, FBAQuorumSet(), s->getClock());

    auto b = TCPPeer::initiate(*s->getNode(n0), string("127.0.0.1"), s->getNode(n1)->getConfig().PEER_PORT);

    while (s->crankAllNodes(1) > 0);
    //s->crankForAtMost(std::chrono::seconds(20));

    LOG(DEBUG) << "done";

}

}
