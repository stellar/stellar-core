#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/StellarXDR.h"

namespace stellar
{

void dumpXdrStream(std::string const& filename);
void printXdr(std::string const& filename, std::string const& filetype,
              bool base64);
void signtxn(std::string const& filename, std::string netId, bool base64);
void priv2pub();
}
