#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <string>
#include <vector>
#include "generated/SCPXDR.h"
#include "crypto/ByteSlice.h"

namespace stellar
{

// Higher-level routines that target the stellar alphabet stellar base58check
// version tags defined here (and a double-sha256 checksum, a la bitcoin); you
// probably want to use these.

typedef enum
{
    VER_NONE = 1,
    VER_ACCOUNT_ID = 0, // 'g'
    VER_SEED = 33       // 's'
} Base58CheckVersionByte;

// Encode a version byte and ByteSlice into Base58Check (with the stellar alphabet).
std::string toBase58Check(Base58CheckVersionByte ver, ByteSlice const& bin);

// Decode a version byte and bytes from Base58Check (with the stellar alphabet).
std::pair<Base58CheckVersionByte, std::vector<uint8_t>>
fromBase58Check(std::string const& encoded);

// Variant that throws when there's either the wrong version byte
// or anything other than 32 bytes encoded.
uint256 fromBase58Check256(Base58CheckVersionByte expect,
                           std::string const& encoded);

// Lower-level helper routines; you probably don't want to use these ones.

std::string baseEncode(std::string const& alphabet, ByteSlice const& bin);

std::vector<uint8_t> baseDecode(std::string const& alphabet,
                                std::string const& encoded);

std::string baseCheckEncode(std::string const& alphabet, uint8_t ver,
                            ByteSlice const& bin);

std::pair<uint8_t, std::vector<uint8_t>>
baseCheckDecode(std::string const& alphabet, std::string const& encoded);

extern const std::string bitcoinBase58Alphabet;
extern const std::string stellarBase58Alphabet;
}
