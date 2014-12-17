#ifndef __TEST__
#define __TEST__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

namespace stellar
{

class Config;

Config const& getTestConfig();
int test(int argc, char* const* argv);
}

#endif
