#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Logging.h"

namespace stellar
{

void fuzz(std::string const& filename, el::Level logLevel,
          std::vector<std::string> const& metrics);
void genfuzz(std::string const& filename);
}
