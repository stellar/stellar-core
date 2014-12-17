#ifndef __TEST__
#define __TEST__

namespace stellar
{

class Config;

Config const& getTestConfig();
int test(int argc, char* const* argv);

}

#endif
