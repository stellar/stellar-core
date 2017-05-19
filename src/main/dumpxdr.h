#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/StellarXDR.h"

namespace stellar
{

extern const char* signtxn_network_id;
void dumpxdr(std::string const& filename);
void printtxn(std::string const& filename, bool base64);
void signtxn(std::string const& filename, bool base64);
void priv2pub();
}
