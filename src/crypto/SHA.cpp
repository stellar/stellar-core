// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

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
    if (crypto_hash_sha256_init(&mState) != 0)
    {
        throw std::runtime_error("error from crypto_hash_sha256_init");
    }
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
