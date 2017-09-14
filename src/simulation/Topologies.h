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

    static Simulation::pointer
    cycle4(Hash const& networkID, std::function<Config()> confGen = nullptr);

    static Simulation::pointer core(uint16_t nNodes, float quorumThresoldFraction,
                                    Simulation::Mode mode,
                                    Hash const& networkID,
                                    std::function<Config()> confGen = nullptr);

    static Simulation::pointer cycle(uint16_t nNodes, float quorumThresoldFraction,
                                     Simulation::Mode mode,
                                     Hash const& networkID,
                                     std::function<Config()> confGen = nullptr);

    static Simulation::pointer
    branchedcycle(uint16_t nNodes, float quorumThresoldFraction,
                  Simulation::Mode mode, Hash const& networkID,
                  std::function<Config()> confGen = nullptr);

    static Simulation::pointer
    separate(uint16_t nNodes, float quorumThresoldFraction, Simulation::Mode mode,
             Hash const& networkID, std::function<Config()> confGen = nullptr);

    static Simulation::pointer
    hierarchicalQuorum(uint16_t nBranches, Simulation::Mode mode,
                       Hash const& networkID,
                       std::function<Config()> confGen = nullptr);
    static Simulation::pointer
    hierarchicalQuorumSimplified(uint16_t coreSize, uint16_t nbOuterNodes,
                                 Simulation::Mode mode, Hash const& networkID,
                                 std::function<Config()> confGen = nullptr);
};
}
