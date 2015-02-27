#pragma once
#include "Simulation.h"

namespace stellar
{

class Topologies
{
public:
    static Simulation::pointer pair(bool overTCP);
    static Simulation::pointer cycle4();
    static Simulation::pointer core4();
};

}
