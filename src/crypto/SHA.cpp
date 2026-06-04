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
#include <openssl/sha.h>
#include <vector>

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

// Plain SHA256. Routed through the Rust sha2 bridge rather than OpenSSL 3.x's
// one-shot ::SHA256(): the latter implicitly fetches the algorithm (taking a
// global lock) on every call and therefore does not scale across threads, which
// throttled the parallel Soroban apply (sha256 is called per-entry/per-tx). The
// Rust impl is stateless/lock-free, uses SHA-NI, and is byte-identical.
uint256
sha256(ByteSlice const& bin)
{
    ZoneScoped;
    uint256 out;
    rust_bridge::sha256_rust(bin.data(), bin.size(), out.data());
    return out;
}

Hash
subSha256(ByteSlice const& seed, uint64_t counter)
{
    // Equivalent to sha256(seed || xdr(counter)); built as one buffer so it goes
    // through the thread-scalable bridged sha256() above. Called per-tx during
    // parallel apply, so avoiding the non-scaling path matters here.
    auto counterBytes = xdr::xdr_to_opaque(counter);
    std::vector<uint8_t> buf;
    buf.reserve(seed.size() + counterBytes.size());
    buf.insert(buf.end(), seed.begin(), seed.end());
    buf.insert(buf.end(), counterBytes.begin(), counterBytes.end());
    return sha256(buf);
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
