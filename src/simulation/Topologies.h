#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "simulation/Simulation.h"

namespace stellar
{

class Topologies
{
  public:
    static Simulation::pointer pair(Simulation::Mode mode);
    static Simulation::pointer cycle4();
    static Simulation::pointer core4(
        Simulation::Mode mode= Simulation::OVER_LOOPBACK);
    static Simulation::pointer core3(
        Simulation::Mode mode = Simulation::OVER_LOOPBACK);
};
}
