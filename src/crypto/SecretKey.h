#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "generated/StellarXDR.h"

namespace stellar
{

class ByteSlice;

struct PublicKey : public uint256
{
    // Return true iff `signature` is valid for `bin` under this key.
    bool verify(uint512 const& signature, ByteSlice const& bin) const;

    // Return true iff `signature` is valid for `bin` under `key`.
    static bool verifySig(const uint256& key, uint512 const& signature,
                          ByteSlice const& bin);
};

class SecretKey : public uint512
{
  public:
    // Get the public key portion of this secret key.
    PublicKey getPublicKey() const;

    // Get the seed portion of this secret key.
    uint256 getSeed() const;

    // Get the seed portion of this secret key as as Base58Check string.
    std::string getBase58Seed() const;

    // Get the public key portion of this secret key as as Base58Check string.
    std::string getBase58Public() const;

    // Return true iff this key is all-zero.
    bool isZero() const;

    // Produce a signature of `bin` using this secret key.
    uint512 sign(ByteSlice const& bin) const;

    // Create a new, random secret key.
    static SecretKey random();

    // Decode a secret key from a provided Base58Check seed value.
    static SecretKey fromBase58Seed(std::string const& base58Seed);

    // Decode a secret key from a binary seed value.
    static SecretKey fromSeed(const uint256& seed);
};
}
