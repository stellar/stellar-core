// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "util/Logging.h"

#include <iosfwd>
#include <string>
#include <vector>

namespace stellar
{

class Config;

struct CommandLineArgs
{
    std::string mExeName;
    std::string mCommandName;
    std::string mCommandDescription;
    std::vector<std::string> mArgs;
};

int handleCommandLine(int argc, char* const* argv);
int runVersion(CommandLineArgs const&);
void writeVersionInfo(std::ostream& os);

void writeWithTextFlow(std::ostream& os, std::string const& text);

#ifdef BUILD_TESTS
// Applies the fixed configuration the `apply-load` command needs (standalone
// mode, manual close, genesis-from-config, ...). It deliberately does NOT set
// NETWORK_PASSPHRASE: that is taken from the operator's config.
void configureApplyLoad(Config& cfg);
#endif
}
