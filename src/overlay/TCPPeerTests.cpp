#include "util/Timer.h"
#include "TCPPeer.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/test.h"
#include "overlay/PeerDoor.h"
#include "main/Config.h"
#include "util/Logging.h"
#include "simulation/Simulation.h"
#include "overlay/PeerMaster.h"

namespace stellar
{


TEST_CASE("TCPPeer can communicate", "[overlay]")
{
    Simulation::pointer s = make_shared<Simulation>(false);

    SIMULATION_CREATE_NODE(0);
    SIMULATION_CREATE_NODE(1);

    auto& n0 = s->getNode(s->addNode(v0VSeed, FBAQuorumSet(), s->getClock()));
    auto& n1 = s->getNode(s->addNode(v1VSeed, FBAQuorumSet(), s->getClock()));
    auto b = TCPPeer::initiate(*n0, "127.0.0.1", n1->getConfig().PEER_PORT);

    n0->enableRealTimer();
    n1->enableRealTimer();

    s->crankForAtLeast(std::chrono::seconds(3));

    REQUIRE(n0->getPeerMaster().getConnectedPeer("127.0.0.1", n1->getConfig().PEER_PORT)->getState() == Peer::GOT_HELLO);
    REQUIRE(n1->getPeerMaster().getConnectedPeer("127.0.0.1", n0->getConfig().PEER_PORT)->getState() == Peer::GOT_HELLO);
}

}
