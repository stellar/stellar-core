#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "xdr/Stellar-types.h"
#include <vector>

namespace stellar
{

struct SCPEnvelope;
struct SCPStatement;
struct StellarValue;

std::vector<Hash> getTxSetHashes(SCPEnvelope const& envelope);
std::vector<StellarValue> getStellarValues(SCPStatement const& envelope);
}
