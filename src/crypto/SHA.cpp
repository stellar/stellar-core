// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SHA.h"
#include "crypto/ByteSlice.h"
#include "util/NonCopyable.h"
#include "util/make_unique.h"
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

SHA256Impl::SHA256Impl() : mFinished(false)
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

// HMAC-SHA256
HmacSha256Mac
hmacSha256(HmacSha256Key const& key, ByteSlice const& bin)
{
    HmacSha256Mac out;
    if (crypto_auth_hmacsha256(out.mac.data(), bin.data(), bin.size(),
                               key.key.data()) != 0)
    {
        throw std::runtime_error("error from crypto_auto_hmacsha256");
    }
    return out;
}

bool
hmacSha256Verify(HmacSha256Mac const& hmac, HmacSha256Key const& key,
                 ByteSlice const& bin)
{
    return 0 == crypto_auth_hmacsha256_verify(hmac.mac.data(), bin.data(),
                                              bin.size(), key.key.data());
}

// Unsalted HKDF-extract(bytes) == HMAC(<zero>,bytes)
HmacSha256Key
hkdfExtract(ByteSlice const& bin)
{
    HmacSha256Key zerosalt;
    auto mac = hmacSha256(zerosalt, bin);
    HmacSha256Key key;
    key.key = mac.mac;
    return key;
}

// Single-step HKDF-expand(key,bytes) == HMAC(key,bytes|0x1)
HmacSha256Key
hkdfExpand(HmacSha256Key const& key, ByteSlice const& bin)
{
    std::vector<uint8_t> bytes(bin.begin(), bin.end());
    bytes.push_back(1);
    auto mac = hmacSha256(key, bytes);
    HmacSha256Key out;
    out.key = mac.mac;
    return out;
}
}
