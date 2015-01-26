#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "generated/StellarXDR.h"
#include "crypto/ByteSlice.h"

namespace stellar
{

std::string
binToHex(ByteSlice const& bin);

std::vector<uint8_t>
hexToBin(std::string const& hex);

// Variant that throws when there's not 32 bytes encoded.
uint256
hexToBin256(std::string const& encoded);

}
