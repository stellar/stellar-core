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
    bool verify(uint512 const& signature, ByteSlice const& bin) const;
    static bool verifySig(const uint256& key, uint512 const& signature,
                          ByteSlice const& bin);
};

class SecretKey : public uint512
{
  public:
    PublicKey getPublicKey() const;
    uint256 getSeed() const;
    std::string getBase58Seed() const;
    std::string getBase58Public() const;
    bool isZero() const;

    uint512 sign(ByteSlice const& bin) const;

    static SecretKey random();
    static SecretKey fromBase58Seed(std::string const& base58Seed);
    static SecretKey fromSeed(const uint256& seed);
};
}
