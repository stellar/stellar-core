// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ByteSliceHasher.h"
#include <sodium.h>

namespace stellar
{
namespace shortHash
{
static unsigned char sKey[crypto_shorthash_KEYBYTES];
void
initialize()
{
    crypto_shorthash_keygen(sKey);
}
uint64_t
computeHash(stellar::ByteSlice const& b)
{
    uint64_t res;
    static_assert(sizeof(res) == crypto_shorthash_BYTES, "unexpected size");
    crypto_shorthash(reinterpret_cast<unsigned char*>(&res),
                     reinterpret_cast<const unsigned char*>(b.data()), b.size(),
                     sKey);
    return res;
}
}
}