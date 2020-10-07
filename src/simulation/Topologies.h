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
    static Simulation::pointer
    pair(Simulation::Mode mode, Hash const& networkID,
         Simulation::ConfigGen confGen = nullptr,
         Simulation::QuorumSetAdjuster qSetAdjust = nullptr);

    // cyclic network - each node has a qset with a neighbor
    static Simulation::pointer
    cycle4(Hash const& networkID, Simulation::ConfigGen confGen = nullptr,
           Simulation::QuorumSetAdjuster qSetAdjust = nullptr);

    // nNodes with same qSet - mesh network
    static Simulation::pointer
    core(int nNodes, double quorumThresoldFraction, Simulation::Mode mode,
         Hash const& networkID, Simulation::ConfigGen confGen = nullptr,
         Simulation::QuorumSetAdjuster qSetAdjust = nullptr);

    // nNodes with same qSet - one way connection in cycle
    static Simulation::pointer
    cycle(int nNodes, double quorumThresoldFraction, Simulation::Mode mode,
          Hash const& networkID, Simulation::ConfigGen confGen = nullptr,
          Simulation::QuorumSetAdjuster qSetAdjust = nullptr);

    // nNodes with same qSet - two way connection = cycle + alt-path
    static Simulation::pointer
    branchedcycle(int nNodes, double quorumThresoldFraction,
                  Simulation::Mode mode, Hash const& networkID,
                  Simulation::ConfigGen confGen = nullptr,
                  Simulation::QuorumSetAdjuster qSetAdjust = nullptr);

    // nNodes with same qSet - no connection created
    static Simulation::pointer
    separate(int nNodes, double quorumThresoldFraction, Simulation::Mode mode,
             Hash const& networkID, Simulation::ConfigGen confGen = nullptr,
             Simulation::QuorumSetAdjuster qSetAdjust = nullptr);

    // multi-tier quorum (core4 + mid-tier nodes that depend on 2 nodes of
    // core4) mid-tier connected round-robin to core4
    static Simulation::pointer hierarchicalQuorum(
        int nBranches, Simulation::Mode mode, Hash const& networkID,
        Simulation::ConfigGen confGen = nullptr, int connectionsToCore = 1,
        Simulation::QuorumSetAdjuster qSetAdjust = nullptr);

    // 2-tier quorum with a variable size core (with 0.75 threshold)
    // and outer-nodes that listen to core & self
    // outer-nodes have connectionsToCore connections to core nodes
    // (round-robin)
    static Simulation::pointer hierarchicalQuorumSimplified(
        int coreSize, int nbOuterNodes, Simulation::Mode mode,
        Hash const& networkID, Simulation::ConfigGen confGen = nullptr,
        int connectionsToCore = 1,
        Simulation::QuorumSetAdjuster qSetAdjust = nullptr);

    // custom-A models a network with 7 nodes A, B, C, T, I, E, S where I is a
    // dead node for resilience tests. The threshold 4 for the qsets of A, B, C
    // is valid for the resilience tests because the resilience tests do not
    // simulate Byzantine failures.
    static Simulation::pointer
    customA(Simulation::Mode mode, Hash const& networkID,
            Simulation::ConfigGen confGen = nullptr, int connections = 1,
            Simulation::QuorumSetAdjuster qSetAdjust = nullptr);

    // Assymetric modifies `core` topology by adding extra nodes to one of the
    // validators in core
    static Simulation::pointer
    assymetric(Simulation::Mode mode, Hash const& networkID,
               Simulation::ConfigGen confGen = nullptr, int connections = 1,
               Simulation::QuorumSetAdjuster qSetAdjust = nullptr);
};
}
