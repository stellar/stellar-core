// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SHA.h"
#include "crypto/ByteSlice.h"
#include "crypto/CryptoError.h"
#include "crypto/Curve25519.h"
#include "util/NonCopyable.h"
#include <Tracy.hpp>
#include <openssl/sha.h>

// Verify that the aligned storage in SHA.h matches the real SHA256_CTX.
static_assert(sizeof(SHA256_CTX) == 112,
              "SHA256_CTX size mismatch with aligned storage in SHA.h");
static_assert(alignof(SHA256_CTX) <= 4,
              "SHA256_CTX alignment exceeds aligned storage in SHA.h");

namespace stellar
{

// Helper to access the OpenSSL SHA256_CTX stored in the aligned byte array.
static inline SHA256_CTX*
ctx(std::byte* s)
{
    return reinterpret_cast<SHA256_CTX*>(s);
}

// Plain SHA256 — use OpenSSL one-shot (auto-selects SHA-NI on supported CPUs).
uint256
sha256(ByteSlice const& bin)
{
    ZoneScoped;
    uint256 out;
    // Use the fully-qualified OpenSSL ::SHA256 to avoid name conflict with
    // stellar::SHA256 class.
    ::SHA256(bin.data(), bin.size(), out.data());
    return out;
}

Hash
subSha256(ByteSlice const& seed, uint64_t counter)
{
    SHA256 sha;
    sha.add(seed);
    sha.add(xdr::xdr_to_opaque(counter));
    return sha.finish();
}

SHA256::SHA256()
{
    reset();
}

void
SHA256::reset()
{
    SHA256_Init(ctx(mState));
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
    SHA256_Update(ctx(mState), bin.data(), bin.size());
}

uint256
SHA256::finish()
{
    uint256 out;
    static_assert(sizeof(out) == SHA256_DIGEST_LENGTH,
                  "unexpected SHA256_DIGEST_LENGTH");
    if (mFinished)
    {
        throw std::runtime_error("finishing already-finished SHA256");
    }
    SHA256_Final(out.data(), ctx(mState));
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
