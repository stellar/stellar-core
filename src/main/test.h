#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC
#include "lib/util/easylogging++.h"

namespace stellar
{

class Config;

Config& getTestConfig();
int test(int argc, char* const* argv,el::Level logLevel);
}


