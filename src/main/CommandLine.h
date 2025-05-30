#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Logging.h"

#include <string>
#include <vector>

namespace stellar
{

struct CommandLineArgs
{
    std::string mExeName;
    std::string mCommandName;
    std::string mCommandDescription;
    std::vector<std::string> mArgs;
};

int handleCommandLine(int argc, char* const* argv);
int runVersion(CommandLineArgs const&);

void writeWithTextFlow(std::ostream& os, std::string const& text);
}
