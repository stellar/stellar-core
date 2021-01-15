// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ShortHash.h"
#include "fmt/format.h"
#include <mutex>
#include <sodium.h>

namespace stellar
{
namespace shortHash
{
static unsigned char gKey[crypto_shorthash_KEYBYTES];
static std::mutex gKeyMutex;
static bool gHaveHashed{false};
static unsigned int gExplicitSeed{0};

void
initialize()
{
    std::lock_guard<std::mutex> guard(gKeyMutex);
    crypto_shorthash_keygen(gKey);
}
#ifdef BUILD_TESTS
void
seed(unsigned int s)
{
    std::lock_guard<std::mutex> guard(gKeyMutex);
    if (gHaveHashed)
    {
        if (gExplicitSeed != s)
        {
            throw std::runtime_error(
                fmt::format("re-seeding shortHash with {} after having already "
                            "hashed with seed {}",
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
    std::lock_guard<std::mutex> guard(gKeyMutex);
    gHaveHashed = true;
    uint64_t res;
    static_assert(sizeof(res) == crypto_shorthash_BYTES, "unexpected size");
    crypto_shorthash(reinterpret_cast<unsigned char*>(&res),
                     reinterpret_cast<const unsigned char*>(b.data()), b.size(),
                     gKey);
    return res;
}

XDRShortHasher::XDRShortHasher() : state(gKey)
{
    std::lock_guard<std::mutex> guard(gKeyMutex);
    gHaveHashed = true;
    state = SipHash24(gKey);
}

void
XDRShortHasher::hashBytes(unsigned char const* bytes, size_t len)
{
    state.update(bytes, len);
}
}
}
