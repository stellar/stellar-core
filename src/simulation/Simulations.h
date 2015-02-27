#pragma once
#include "Simulation.h"

namespace stellar
{

class Simulations
{
public:
    static Simulation::pointer pair();
    static Simulation::pointer cycle4();
    static Simulation::pointer core4();
};

}
