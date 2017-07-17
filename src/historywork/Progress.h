// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <string>

namespace stellar
{

class Application;

std::string fmtProgress(Application& app, std::string const& task,
                        uint32_t first, uint32_t last, uint32_t curr);
}
