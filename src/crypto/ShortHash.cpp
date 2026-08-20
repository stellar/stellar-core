// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ShortHash.h"
#include <sodium.h>

#include <algorithm>
#include <array>
#include <iterator>

#ifdef BUILD_TESTS
#include "fmt/format.h"
#include <atomic>
#endif

namespace stellar
{
namespace shortHash
{
// gKey is written once by initialize() at startup, before any hashing thread
// is spawned.
// In test builds `seed()` may change this between test runs, on the main
// thread while no hashing is in flight. Thus we never need to synchronize
// access to gKey.
static unsigned char gKey[crypto_shorthash_KEYBYTES];

void
initialize()
{
    crypto_shorthash_keygen(gKey);
}

std::array<unsigned char, crypto_shorthash_KEYBYTES>
getShortHashInitKey()
{
    std::array<unsigned char, crypto_shorthash_KEYBYTES> arr;
    std::copy(std::begin(gKey), std::end(gKey), arr.begin());
    return arr;
}

#ifdef BUILD_TESTS
// Tracks whether hashing has already occurred so that `seed()` can reject
// re-seeding with a different seed after hashes have already been computed
// Only ever transitions false->true while hashing.
static std::atomic<bool> gHaveHashed{false};
static unsigned int gExplicitSeed{0};

static void
noteHashed()
{
    if (!gHaveHashed.load(std::memory_order_relaxed))
    {
        gHaveHashed.store(true, std::memory_order_relaxed);
    }
}

void
seed(unsigned int s)
{
    if (gHaveHashed.load(std::memory_order_relaxed))
    {
        if (gExplicitSeed != s)
        {
            throw std::runtime_error(fmt::format(
                FMT_STRING(
                    "re-seeding shortHash with {:d} after having already "
                    "hashed with seed {:d}"),
                s, gExplicitSeed));
        }
    }
    gExplicitSeed = s;
    for (size_t i = 0; i < crypto_shorthash_KEYBYTES; ++i)
    {
        size_t shift = i % sizeof(unsigned int);
        gKey[i] = static_cast<unsigned char>(s >> shift);
    }
}
#endif

uint64_t
computeHash(stellar::ByteSlice const& b)
{
#ifdef BUILD_TESTS
    noteHashed();
#endif
    uint64_t res;
    static_assert(sizeof(res) == crypto_shorthash_BYTES, "unexpected size");
    crypto_shorthash(reinterpret_cast<unsigned char*>(&res),
                     reinterpret_cast<unsigned char const*>(b.data()), b.size(),
                     gKey);
    return res;
}

XDRShortHasher::XDRShortHasher() : state(gKey)
{
#ifdef BUILD_TESTS
    noteHashed();
#endif
}

void
XDRShortHasher::hashBytes(unsigned char const* bytes, size_t len)
{
    state.update(bytes, len);
}
}
}
