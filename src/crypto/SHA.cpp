// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SHA.h"
#include "crypto/ByteSlice.h"
#include "crypto/CryptoError.h"
#include "crypto/Curve25519.h"
#include "rust/RustBridge.h"
#include "util/NonCopyable.h"
#include <Tracy.hpp>
#include <sodium.h>
#include <vector>

namespace stellar
{

// Plain SHA256, routed through the Rust bridge.
uint256
sha256(ByteSlice const& bin)
{
    ZoneScoped;
    uint256 out;
    rust_bridge::compute_sha256(bin.data(), bin.size(), out.data());
    return out;
}

Hash
subSha256(ByteSlice const& seed, uint64_t counter)
{
    ZoneScoped;
    SHA256 sha;
    sha.add(seed);
    sha.add(xdr::xdr_to_opaque(counter));
    return sha.finish();
}

class SHA256::Impl
{
  public:
    Impl() : rust_impl(rust_bridge::new_rust_sha256())
    {
    }
    rust::Box<rust_bridge::RustSha256> rust_impl;
};

SHA256::SHA256() : mImpl(std::make_unique<SHA256::Impl>())
{
}

SHA256::~SHA256() = default;

void
SHA256::reset()
{
    mImpl->rust_impl->reset();
    mFinished = false;
}

void
SHA256::add(ByteSlice const& bin)
{
    ZoneScoped;
    if (mFinished)
    {
        throw std::runtime_error("adding bytes to finished SHA256");
    }
    mImpl->rust_impl->update(bin.data(), bin.size());
}

uint256
SHA256::finish()
{
    if (mFinished)
    {
        throw std::runtime_error("finishing already-finished SHA256");
    }
    uint256 out;
    mImpl->rust_impl->finalize(out.data());
    mFinished = true;
    return out;
}

// HMAC-SHA256
HmacSha256Mac
hmacSha256(HmacSha256Key const& key, ByteSlice const& bin)
{
    ZoneScoped;
    HmacSha256Mac out;
    if (crypto_auth_hmacsha256(out.mac.data(), bin.data(), bin.size(),
                               key.key.data()) != 0)
    {
        throw CryptoError("error from crypto_auto_hmacsha256");
    }
    return out;
}

bool
hmacSha256Verify(HmacSha256Mac const& hmac, HmacSha256Key const& key,
                 ByteSlice const& bin)
{
    ZoneScoped;
    return 0 == crypto_auth_hmacsha256_verify(hmac.mac.data(), bin.data(),
                                              bin.size(), key.key.data());
}

// Unsalted HKDF-extract(bytes) == HMAC(<zero>,bytes)
HmacSha256Key
hkdfExtract(ByteSlice const& bin)
{
    ZoneScoped;
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
    ZoneScoped;
    std::vector<uint8_t> bytes(bin.begin(), bin.end());
    bytes.push_back(1);
    auto mac = hmacSha256(key, bytes);
    HmacSha256Key out;
    out.key = mac.mac;
    return out;
}
}
