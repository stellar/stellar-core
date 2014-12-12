#ifndef __TEST_H__
#define __TEST_H__

namespace stellar
{

class Config;

Config const& getTestConfig();
int test(int argc, char* const* argv);

}

#endif
