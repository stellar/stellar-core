// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "crypto/SHA.h"
#include "crypto/ByteSlice.h"
#include <sodium.h>

namespace stellar
{

// Plain SHA256
uint256
sha256(ByteSlice const& bin)
{
    uint256 out;
    if (crypto_hash_sha256(out.data(), bin.data(), bin.size()) != 0)
    {
        throw std::runtime_error("error from crypto_hash_sha256");
    }
    return out;
}

struct SHA256::Impl
{
    crypto_hash_sha256_state mState;
    bool mFinished;

    Impl() : mFinished(false)
    {
        if (crypto_hash_sha256_init(&mState) != 0)
        {
            throw std::runtime_error("error from crypto_hash_sha256_init");
        }
    }
};

SHA256::SHA256() : mImpl(new Impl())
{
}

SHA256::~SHA256()
{
}

void
SHA256::add(ByteSlice const& bin)
{
    if (mImpl->mFinished)
    {
        throw std::runtime_error("adding bytes to finished SHA256");
    }
    if (crypto_hash_sha256_update(&mImpl->mState, bin.data(), bin.size()) != 0)
    {
        throw std::runtime_error("error from crypto_hash_sha256_update");
    }
}

uint256
SHA256::finish()
{
    unsigned char out[crypto_hash_sha256_BYTES];
    if (mImpl->mFinished)
    {
        throw std::runtime_error("finishing already-finished SHA256");
    }
    if (crypto_hash_sha256_final(&mImpl->mState, out) != 0)
    {
        throw std::runtime_error("error from crypto_hash_sha256_final");
    }
    uint256 trunc;
    std::copy(out, out + crypto_hash_sha256_BYTES, trunc.begin());
    return trunc;
}
}
