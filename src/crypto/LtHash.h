#pragma once

// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/ByteSlice.h"
#include "xdr/Stellar-types.h"
#include <array>
#include <cstdint>

namespace stellar
{

// A commutative ("homomorphic", multiset) hash: the hash of a collection of
// byte strings that does not depend on the order in which they are added, and
// from which a member can be taken away again.
//
// The construction is LtHash16 with 1024 elements (Bellare and Micciancio's
// lattice hash; the parameters of Lewi, Kim, Maykov and Weis, "Securing
// Update Propagation with Homomorphic Hashing", 2019). Every byte string is
// expanded to 1024 16-bit numbers; the state is the element-wise sum, modulo
// 2^16, of the expansions of the members. Two collections hash alike exactly
// when their sums agree, so
//
//     hash(A union B) = hash(A) + hash(B)
//
// for collections with multiplicity, whatever the order and however the
// members are split between A and B. Finding two different collections with
// the same sum is a short-vector problem in a lattice of dimension 1024.
//
// The expansion of a byte string x is the ChaCha20 key stream (IETF variant,
// zero nonce) under the key SHA256("stellar lthash16 v1" || x), read as 1024
// little-endian 16-bit numbers.
//
// The state is 2048 bytes. `digest` condenses it to 32 bytes with SHA-256 for
// use where a hash of that size is expected; sums are formed on states, never
// on digests.
class LtHash16
{
  public:
    static constexpr size_t ELEMENTS = 1024;
    static constexpr size_t STATE_BYTES = 2 * ELEMENTS;

    // The hash of the empty collection: all zero.
    LtHash16();

    // Add or take away one member.
    void add(ByteSlice const& member);
    void remove(ByteSlice const& member);

    // Add or take away a whole collection, given by its hash.
    void add(LtHash16 const& other);
    void remove(LtHash16 const& other);

    LtHash16&
    operator+=(LtHash16 const& other)
    {
        add(other);
        return *this;
    }
    LtHash16&
    operator-=(LtHash16 const& other)
    {
        remove(other);
        return *this;
    }

    bool isZero() const;
    bool operator==(LtHash16 const& other) const;
    bool
    operator!=(LtHash16 const& other) const
    {
        return !(*this == other);
    }

    // SHA-256 of the state, the elements as little-endian 16-bit numbers.
    uint256 digest() const;

    // The state as bytes (little-endian elements), and back.
    std::array<uint8_t, STATE_BYTES> toBytes() const;
    static LtHash16 fromBytes(std::array<uint8_t, STATE_BYTES> const& bytes);

  private:
    std::array<uint16_t, ELEMENTS> mElements;

    static void expand(ByteSlice const& member,
                       std::array<uint16_t, ELEMENTS>& out);
};
}
