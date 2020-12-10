#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

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
std::unique_ptr<Fuzzer> createFuzzer(int processID, FuzzerMode fuzzerMode);
}

void fuzz(std::string const& filename, std::vector<std::string> const& metrics,
          int processID, FuzzerMode fuzzerMode);
}
