#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/ByteSlice.h"
#include "crypto/XDRHasher.h"
#include "util/siphash.h"

namespace stellar
{

// shortHash provides a fast and relatively secure *randomized* hash function
// this is suitable for keeping objects in memory but not for persisting objects
// or cryptographic use
namespace shortHash
{
void initialize();
#ifdef BUILD_TESTS
void seed(unsigned int);
#endif
uint64_t computeHash(stellar::ByteSlice const& b);

struct XDRShortHasher : XDRHasher<XDRShortHasher>
{
    SipHash24 state;
    XDRShortHasher();
    void hashBytes(unsigned char const*, size_t);
};

// Equivalent to `computeHash(xdr_to_opaque(t))` on any XDR object `t` but
// without allocating a temporary buffer. Runs the same (SipHash2,4) short-hash
// function, randomized with the same per-process key as `computeHash`. Uses
// a different implementation, but results are (unit-tested to be) identical.
//
// NB: This is not an overload of `computeHash` to avoid ambiguity when called
// with xdrpp-provided types like opaque_vec, which will convert to a ByteSlice
// if demanded, but can also be passed to XDRShortHasher. Depending on which it
// goes to, it'll hash differently: length+body (XDR case) or just body
// (ByteSlice case). This difference isn't a security feature or anything
// (SipHash integrates length into the hash) but it's a source of potential
// bugs, so we avoid it by using a different function name.
template <typename T>
uint64_t
xdrComputeHash(T const& t)
{
    XDRShortHasher xsh;
    xdr::archive(xsh, t);
    xsh.flush();
    return xsh.state.digest();
}
}
}
