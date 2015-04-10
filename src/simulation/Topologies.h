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
    static Simulation::pointer pair(Simulation::Mode mode);
    static Simulation::pointer cycle4();
    static Simulation::pointer core(int nNodes, float quorumThresoldFraction, Simulation::Mode mode);
    static Simulation::pointer hierarchicalQuorum(int nBranches);
};
}
