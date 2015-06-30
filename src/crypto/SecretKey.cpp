// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SecretKey.h"
#include "crypto/Base58.h"
#include "crypto/Hex.h"
#include <sodium.h>
#include <type_traits>

namespace stellar
{

SecretKey::SecretKey() : mKeyType(KEY_TYPES_ED25519)
{
static_assert(crypto_sign_PUBLICKEYBYTES == sizeof(uint256),
              "Unexpected public key length");
static_assert(crypto_sign_SECRETKEYBYTES == sizeof(uint512),
              "Unexpected secret key length");
static_assert(crypto_sign_BYTES == sizeof(uint512),
              "Unexpected signature length");
}

PublicKey
SecretKey::getPublicKey() const
{
    PublicKey pk;

    assert(mKeyType == KEY_TYPES_ED25519);

    if (crypto_sign_ed25519_sk_to_pk(pk.ed25519().data(), mSecretKey.data()) !=
        0)
    {
        throw std::runtime_error("error extracting public key from secret key");
    }
    return pk;
}

uint256
SecretKey::getSeed() const
{
    assert(mKeyType == KEY_TYPES_ED25519);

    uint256 seed;
    if (crypto_sign_ed25519_sk_to_seed(seed.data(), mSecretKey.data()) != 0)
    {
        throw std::runtime_error("error extracting seed from secret key");
    }
    return seed;
}

std::string
SecretKey::getBase58Seed() const
{
    assert(mKeyType == KEY_TYPES_ED25519);

    return toBase58Check(VER_SEED, getSeed());
}

std::string
SecretKey::getBase58Public() const
{
    return PubKeyUtils::toBase58(getPublicKey());
}

bool
SecretKey::isZero() const
{
    for (auto i : mSecretKey)
    {
        if (i != 0)
        {
            return false;
        }
    }
    return true;
}

Signature
SecretKey::sign(ByteSlice const& bin) const
{
    assert(mKeyType == KEY_TYPES_ED25519);

    Signature out(crypto_sign_BYTES, 0);
    if (crypto_sign_detached(out.data(), NULL, bin.data(), bin.size(),
                             mSecretKey.data()) != 0)
    {
        throw std::runtime_error("error while signing");
    }
    return out;
}

SecretKey
SecretKey::random()
{
    PublicKey pk;
    SecretKey sk;
    assert(sk.mKeyType == KEY_TYPES_ED25519);
    if (crypto_sign_keypair(pk.ed25519().data(), sk.mSecretKey.data()) != 0)
    {
        throw std::runtime_error("error generating random secret key");
    }
    return sk;
}

SecretKey
SecretKey::fromSeed(uint256 const& seed)
{
    PublicKey pk;
    SecretKey sk;
    assert(sk.mKeyType == KEY_TYPES_ED25519);

    if (crypto_sign_seed_keypair(pk.ed25519().data(), sk.mSecretKey.data(),
                                 (unsigned char*)&(seed[0])) != 0)
    {
        throw std::runtime_error("error generating secret key from seed");
    }
    return sk;
}

SecretKey
SecretKey::fromBase58Seed(std::string const& base58Seed)
{
    auto pair = fromBase58Check(base58Seed);
    if (pair.first != VER_SEED)
    {
        throw std::runtime_error(
            "unexpected version byte on secret key base58 seed");
    }

    if (pair.second.size() != crypto_sign_SEEDBYTES)
    {
        throw std::runtime_error(
            "unexpected base58 seed length for secret key");
    }

    PublicKey pk;
    SecretKey sk;
    assert(sk.mKeyType == KEY_TYPES_ED25519);
    if (crypto_sign_seed_keypair(pk.ed25519().data(), sk.mSecretKey.data(),
                                 pair.second.data()) != 0)
    {
        throw std::runtime_error("error generating secret key from seed");
    }
    return sk;
}

bool
PubKeyUtils::verifySig(PublicKey const& key, Signature const& signature,
                       ByteSlice const& bin)
{
    return crypto_sign_verify_detached(signature.data(), bin.data(), bin.size(),
                                       key.ed25519().data()) == 0;
}

std::string
PubKeyUtils::toShortString(PublicKey const& pk)
{
    return hexAbbrev(pk.ed25519());
}

std::string
PubKeyUtils::toBase58(PublicKey const& pk)
{
    // uses VER_ACCOUNT_ID prefix for ed25519
    return toBase58Check(VER_ACCOUNT_ID, pk.ed25519());
}

PublicKey
PubKeyUtils::fromBase58(std::string const& s)
{
    PublicKey pk;
    pk.ed25519() = fromBase58Check256(VER_ACCOUNT_ID, s);
    return pk;
}

SignatureHint
PubKeyUtils::getHint(PublicKey const& pk)
{
    SignatureHint res;
    memcpy(res.data(), &pk.ed25519().back() - res.size(), sizeof(res));
    return res;
}

bool
PubKeyUtils::hasHint(PublicKey const& pk, SignatureHint const& hint)
{
    return memcmp(&pk.ed25519().back() - hint.size(), hint.data(),
                  sizeof(hint)) == 0;
}
}
