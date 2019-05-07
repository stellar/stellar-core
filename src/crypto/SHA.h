#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/ByteSlice.h"
#include "crypto/XDRHasher.h"
#include "xdr/Stellar-types.h"
#include <memory>

namespace stellar
{

// Plain SHA256
uint256 sha256(ByteSlice const& bin);

// SHA256 in incremental mode, for large inputs.
class SHA256
{
  public:
    static std::unique_ptr<SHA256> create();
    virtual ~SHA256(){};
    virtual void reset() = 0;
    virtual void add(ByteSlice const& bin) = 0;
    virtual uint256 finish() = 0;
};

// Helper for xdrSha256 below.
struct XDRSHA256 : XDRHasher<XDRSHA256>
{
    std::unique_ptr<SHA256> state;
    XDRSHA256() : state(SHA256::create())
    {
    }
    void
    hashBytes(unsigned char const* bytes, size_t size)
    {
        state->add(ByteSlice(bytes, size));
    }
};

// Equivalent to `sha256(xdr_to_opaque(t))` on any XDR object `t` but
// without allocating a temporary buffer.
//
// NB: This is not an overload of `sha256` to avoid ambiguity when called
// with xdrpp-provided types like opaque_vec, which will convert to a ByteSlice
// if demanded, but can also be passed to XDRSHA256.
template <typename T>
uint256
xdrSha256(T const& t)
{
    XDRSHA256 xs;
    xdr::archive(xs, t);
    xs.flush();
    return xs.state->finish();
}

// HMAC-SHA256 (keyed)
HmacSha256Mac hmacSha256(HmacSha256Key const& key, ByteSlice const& bin);

// Use this rather than HMAC-output ==, to avoid timing leaks.
bool hmacSha256Verify(HmacSha256Mac const& hmac, HmacSha256Key const& key,
                      ByteSlice const& bin);

// Unsalted HKDF-extract(bytes) == HMAC(<zero>,bytes)
HmacSha256Key hkdfExtract(ByteSlice const& bin);

// Single-step HKDF-expand(key,bytes) == HMAC(key,bytes|0x1)
HmacSha256Key hkdfExpand(HmacSha256Key const& key, ByteSlice const& bin);
}
