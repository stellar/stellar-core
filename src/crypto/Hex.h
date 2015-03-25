#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "generated/StellarXDR.h"
#include "crypto/ByteSlice.h"

namespace stellar
{

// Hex-encode a ByteSlice.
std::string binToHex(ByteSlice const& bin);

// Hex-encode a ByteSlice and return a 6-character prefix of it (for logging).
std::string hexAbbrev(ByteSlice const& bin);

// Hex-decode bytes from a hex string.
std::vector<uint8_t> hexToBin(std::string const& hex);

// Hex-decode exactly 32 bytes from a hex string, throw if not 32 bytes.
uint256 hexToBin256(std::string const& encoded);
}
