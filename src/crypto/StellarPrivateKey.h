#ifndef __STELLARPRIVATEKEY__
#define __STELLARPRIVATEKEY__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "crypto/EdKeyPair.h"
#include "util/types.h"
#include "generated/StellarXDR.h"

/*
one half of the signing key
*/
namespace stellar
{

class StellarPrivateKey
{
  protected:
    Blob mSeed;
    EdKeyPair mPair;
    // RippleAddress::VersionEncoding mType;
  public:
    StellarPrivateKey();

    void fromRandomness();
    void fromPassPhrase(std::string& passPhrase);
    bool fromString(std::string& base58seed);

    void sign(stellarxdr::uint256 const& message, Blob& retSignature) const;

    std::string base58Seed() const;
    std::string base58AccountID() const;
    virtual std::string base58PublicKey() const = 0;
    std::string hexPublicKey() const;

    stellarxdr::uint160 getAccountID() const;
    Blob&
    getPublicKey()
    {
        return (mPair.mPublicKey);
    }
    bool
    isValid()
    {
        return (mSeed.size() != 0);
    }
};

class NodePrivateKey : public StellarPrivateKey
{
  public:
    std::string base58PublicKey() const;
};

class AccountPrivateKey : public StellarPrivateKey
{
  public:
    std::string base58PublicKey() const;
};
}

#endif
