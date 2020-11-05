// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/BLAKE2.h"
#include "crypto/ByteSlice.h"
#include "crypto/CryptoError.h"
#include "util/NonCopyable.h"
#include <Tracy.hpp>
#include <sodium.h>

namespace stellar
{

// Plain BLAKE2 (a.k.a. BLAKE2b)
uint256
blake2(ByteSlice const& bin)
{
    static_assert(crypto_generichash_BYTES == sizeof(uint256),
                  "unexpected crypto_generichash_BYTES");
    ZoneScoped;
    uint256 out;
    if (crypto_generichash(out.data(), out.size(), bin.data(), bin.size(),
                           nullptr, 0) != 0)
    {
        throw CryptoError("error from crypto_generichash");
    }
    return out;
}

BLAKE2::BLAKE2()
{
    reset();
}

void
BLAKE2::reset()
{
    if (crypto_generichash_init(&mState, nullptr, 0,
                                crypto_generichash_BYTES) != 0)
    {
        throw CryptoError("error from crypto_generichash_init");
    }
    mFinished = false;
}

void
BLAKE2::add(ByteSlice const& bin)
{
    ZoneScoped;
    if (mFinished)
    {
        throw std::runtime_error("adding bytes to finished BLAKE2");
    }
    if (crypto_generichash_update(&mState, bin.data(), bin.size()) != 0)
    {
        throw CryptoError("error from crypto_generichash_update");
    }
}

uint256
BLAKE2::finish()
{
    uint256 out;
    if (mFinished)
    {
        throw std::runtime_error("finishing already-finished BLAKE2");
    }
    if (crypto_generichash_final(&mState, out.data(), out.size()) != 0)
    {
        throw CryptoError("error from crypto_generichash_final");
    }
    return out;
}
}
