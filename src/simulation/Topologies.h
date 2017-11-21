#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "simulation/Simulation.h"

namespace stellar
{

class Topologies
{
  public:
    static Simulation::pointer pair(Simulation::Mode mode,
                                    Hash const& networkID,
                                    std::function<Config()> confGen = nullptr);

    // cyclic network - each node has a qset with a neighbor
    static Simulation::pointer
    cycle4(Hash const& networkID, std::function<Config()> confGen = nullptr);

    // nNodes with same qSet - mesh network
    static Simulation::pointer core(int nNodes, double quorumThresoldFraction,
                                    Simulation::Mode mode,
                                    Hash const& networkID,
                                    std::function<Config()> confGen = nullptr);

    // nNodes with same qSet - one way connection in cycle
    static Simulation::pointer cycle(int nNodes, double quorumThresoldFraction,
                                     Simulation::Mode mode,
                                     Hash const& networkID,
                                     std::function<Config()> confGen = nullptr);

    // nNodes with same qSet - two way connection = cycle + alt-path
    static Simulation::pointer
    branchedcycle(int nNodes, double quorumThresoldFraction,
                  Simulation::Mode mode, Hash const& networkID,
                  std::function<Config()> confGen = nullptr);

    // nNodes with same qSet - no connection created
    static Simulation::pointer
    separate(int nNodes, double quorumThresoldFraction, Simulation::Mode mode,
             Hash const& networkID, std::function<Config()> confGen = nullptr);

    // multi-tier quorum (core4 + mid-tier nodes that depend on 2 nodes of
    // core4) mid-tier connected round-robin to core4
    static Simulation::pointer
    hierarchicalQuorum(int nBranches, Simulation::Mode mode,
                       Hash const& networkID,
                       std::function<Config()> confGen = nullptr);

    // 2-tier quorum with a variable size core and outer-nodes that listen to
    // core & self outer-nodes have a single connection to one of the core nodes
    // (round-robin)
    static Simulation::pointer
    hierarchicalQuorumSimplified(int coreSize, int nbOuterNodes,
                                 Simulation::Mode mode, Hash const& networkID,
                                 std::function<Config()> confGen = nullptr);
};
}
