// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SHA.h"
#include "crypto/ByteSlice.h"
#include <sodium.h>
#include "util/make_unique.h"
#include "util/NonCopyable.h"

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

class SHA256Impl : public SHA256, NonCopyable
{
    crypto_hash_sha256_state mState;
    bool mFinished;
public:
    SHA256Impl();
    void reset() override;
    void add(ByteSlice const& bin) override;
    uint256 finish() override;
};

std::unique_ptr<SHA256>
SHA256::create()
{
    return make_unique<SHA256Impl>();
}

SHA256Impl::SHA256Impl()
    : mFinished(false)
{
    reset();
}

void
SHA256Impl::reset()
{
    if (crypto_hash_sha256_init(&mState) != 0)
    {
        throw std::runtime_error("error from crypto_hash_sha256_init");
    }
    mFinished = false;
}

void
SHA256Impl::add(ByteSlice const& bin)
{
    if (mFinished)
    {
        throw std::runtime_error("adding bytes to finished SHA256");
    }
    if (crypto_hash_sha256_update(&mState, bin.data(), bin.size()) != 0)
    {
        throw std::runtime_error("error from crypto_hash_sha256_update");
    }
}

uint256
SHA256Impl::finish()
{
    uint256 out;
    assert(out.size() == crypto_hash_sha256_BYTES);
    if (mFinished)
    {
        throw std::runtime_error("finishing already-finished SHA256");
    }
    if (crypto_hash_sha256_final(&mState, out.data()) != 0)
    {
        throw std::runtime_error("error from crypto_hash_sha256_final");
    }
    return out;
}
}
