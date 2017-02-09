#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Config.h"
#include "util/Logging.h"

namespace stellar
{

class Config;

Config const& getTestConfig(int instanceNumber = 0,
                            Config::TestDbMode mode = Config::TESTDB_DEFAULT);
int test(int argc, char* const* argv, el::Level logLevel,
         std::vector<std::string> const& metrics);

extern bool force_sqlite;
}
