// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ShortHash.h"
#include "fmt/format.h"
#include <atomic>
#include <mutex>
#include <sodium.h>

namespace stellar
{
namespace shortHash
{
// gKey is written only by initialize() (at startup, before any concurrent
// hashing) and seed() (test-only, between test runs). The hashing fast paths
// below read it without synchronization: taking a lock per hash serializes
// every hash-keyed container operation in the process across all threads,
// which measurably destroys the scaling of parallel transaction apply.
static unsigned char gKey[crypto_shorthash_KEYBYTES];
static std::mutex gKeyMutex;
#ifdef BUILD_TESTS
// Tracked so that seed() can reject re-seeding with a different seed after
// hashes have already been computed. Only ever transitions false->true while
// hashing, and the hash paths store to it only when it isn't already set, so
// in steady state the cache line stays shared across cores instead of
// bouncing.
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
#else
static void
noteHashed()
{
}
#endif

void
initialize()
{
    std::lock_guard<std::mutex> guard(gKeyMutex);
    crypto_shorthash_keygen(gKey);
}

std::array<unsigned char, crypto_shorthash_KEYBYTES>
getShortHashInitKey()
{
    std::lock_guard<std::mutex> guard(gKeyMutex);
    std::array<unsigned char, crypto_shorthash_KEYBYTES> arr;
    std::copy(std::begin(gKey), std::end(gKey), arr.begin());
    return arr;
}

#ifdef BUILD_TESTS
void
seed(unsigned int s)
{
    std::lock_guard<std::mutex> guard(gKeyMutex);
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
    noteHashed();
    uint64_t res;
    static_assert(sizeof(res) == crypto_shorthash_BYTES, "unexpected size");
    crypto_shorthash(reinterpret_cast<unsigned char*>(&res),
                     reinterpret_cast<unsigned char const*>(b.data()), b.size(),
                     gKey);
    return res;
}

XDRShortHasher::XDRShortHasher() : state(gKey)
{
    noteHashed();
}

void
XDRShortHasher::hashBytes(unsigned char const* bytes, size_t len)
{
    state.update(bytes, len);
}
}
}
