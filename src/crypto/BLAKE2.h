#pragma once

// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/ByteSlice.h"
#include "crypto/XDRHasher.h"
#include "sodium/crypto_generichash.h"
#include "xdr/Stellar-types.h"
#include <memory>

namespace stellar
{

// Plain BLAKE2 (a.k.a. BLAKE2b)
uint256 blake2(ByteSlice const& bin);

// BLAKE2 in incremental mode, for large inputs.
class BLAKE2
{
    crypto_generichash_state mState;
    bool mFinished{false};

  public:
    BLAKE2();
    void reset();
    void add(ByteSlice const& bin);
    uint256 finish();
};

// Helper for xdrBlake2 below.
struct XDRBLAKE2 : XDRHasher<XDRBLAKE2>
{
    BLAKE2 state;
    void
    hashBytes(unsigned char const* bytes, size_t size)
    {
        state.add(ByteSlice(bytes, size));
    }
};

// Equivalent to `blake2(xdr_to_opaque(t))` on any XDR object `t` but
// without allocating a temporary buffer.
//
// NB: This is not an overload of `blake2` to avoid ambiguity when called
// with xdrpp-provided types like opaque_vec, which will convert to a ByteSlice
// if demanded, but can also be passed to XDRBLAKE2.
template <typename T>
uint256
xdrBlake2(T const& t)
{
    XDRBLAKE2 xb;
    xdr::archive(xb, t);
    xb.flush();
    return xb.state.finish();
}
}
