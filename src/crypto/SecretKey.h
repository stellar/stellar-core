#ifndef __SIGN__
#define __SIGN__

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
};

class SecretKey : public uint512
{
    SecretKey();
public:
    PublicKey getPublicKey() const;
    std::string getBase58Seed() const;

    uint512 sign(ByteSlice const& bin) const;

    static SecretKey random();
    static SecretKey fromBase58Seed(std::string const& base58Seed);
};

}

#endif
