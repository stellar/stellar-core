#pragma once
#include "Simulation.h"

namespace stellar
{

class Topologies
{
  public:
    static Simulation::pointer pair(Simulation::Mode mode);
    static Simulation::pointer cycle4();
    static Simulation::pointer core4();
};
}
