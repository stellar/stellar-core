#pragma once
// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ByteSlice.h"
#include <string>

namespace stellar
{

struct SecretValue;

namespace strKey
{

enum StrKeyVersionByte : uint8_t
{
    // version bytes - 5 bits only
    STRKEY_PUBKEY_ED25519 = 6, // 'G'
    STRKEY_SEED_ED25519 = 18,  // 'S'
    STRKEY_PRE_AUTH_TX = 19,   // 'T',
    STRKEY_HASH_X = 23         // 'X'
};

// Encode a version byte and ByteSlice into StrKey
SecretValue toStrKey(uint8_t ver, ByteSlice const& bin);

// computes the size of the StrKey that would result from encoding
// a ByteSlice of dataSize bytes
size_t getStrKeySize(size_t dataSize);

// returns true if the strKey could be decoded
bool fromStrKey(std::string const& strKey, uint8_t& outVersion,
                std::vector<uint8_t>& decoded);
}
}
