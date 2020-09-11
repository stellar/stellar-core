#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Logging.h"
#include <memory>
#include <string>
#include <vector>

namespace stellar
{

class Fuzzer;

enum class FuzzerMode
{
    OVERLAY,
    TRANSACTION
};

namespace FuzzUtils
{
extern unsigned int const NUMBER_OF_PREGENERATED_ACCOUNTS;
std::unique_ptr<Fuzzer> createFuzzer(int processID, FuzzerMode fuzzerMode);
}

void fuzz(std::string const& filename, el::Level logLevel,
          std::vector<std::string> const& metrics, int processID,
          FuzzerMode fuzzerMode);
}
