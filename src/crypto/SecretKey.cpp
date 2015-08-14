// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SecretKey.h"
#include "crypto/Base58.h"
#include "crypto/StrKey.h"
#include "crypto/Hex.h"
#include <sodium.h>
#include <type_traits>
#include <memory>
#include <util/make_unique.h>
#include <mutex>

#include "util/lrucache.hpp"

namespace stellar
{

// Process-wide global Ed25519 signature-verification cache.
//
// This is a pure mathematical function and has no relationship
// to the state of the process; caching its results centrally
// makes all signature-verification in the program faster and
// has no effect on correctness.

static std::mutex gVerifySigCacheMutex;
static cache::lru_cache<std::string, bool> gVerifySigCache(4096);

static bool
shouldCacheVerifySig(PublicKey const& key, Signature const& signature,
                     ByteSlice const& bin)
{
    return (bin.size() < 0xffff);
}

static std::string
verifySigCacheKey(PublicKey const& key, Signature const& signature,
                  ByteSlice const& bin)
{
    return (binToHex(key.ed25519())
            + ":" + binToHex(signature)
            + ":" + binToHex(bin));
}


SecretKey::SecretKey() : mKeyType(KEY_TYPE_ED25519)
{
    static_assert(crypto_sign_PUBLICKEYBYTES == sizeof(uint256),
                  "Unexpected public key length");
    static_assert(crypto_sign_SEEDBYTES == sizeof(uint256),
                  "Unexpected seed length");
    static_assert(crypto_sign_SECRETKEYBYTES == sizeof(uint512),
                  "Unexpected secret key length");
    static_assert(crypto_sign_BYTES == sizeof(uint512),
                  "Unexpected signature length");
}

PublicKey
SecretKey::getPublicKey() const
{
    PublicKey pk;

    assert(mKeyType == KEY_TYPE_ED25519);

    if (crypto_sign_ed25519_sk_to_pk(pk.ed25519().data(), mSecretKey.data()) !=
        0)
    {
        throw std::runtime_error("error extracting public key from secret key");
    }
    return pk;
}

SecretKey::Seed
SecretKey::getSeed() const
{
    assert(mKeyType == KEY_TYPE_ED25519);

    Seed seed;
    seed.mKeyType = mKeyType;
    if (crypto_sign_ed25519_sk_to_seed(seed.mSeed.data(), mSecretKey.data()) !=
        0)
    {
        throw std::runtime_error("error extracting seed from secret key");
    }
    return seed;
}

std::string
SecretKey::getStrKeySeed() const
{
    assert(mKeyType == KEY_TYPE_ED25519);

    return strKey::toStrKey(strKey::STRKEY_SEED_ED25519, getSeed().mSeed);
}

std::string
SecretKey::getStrKeyPublic() const
{
    return PubKeyUtils::toStrKey(getPublicKey());
}

std::string
SecretKey::getBase58Seed() const
{
    assert(mKeyType == KEY_TYPE_ED25519);

    return toBase58Check(B58_SEED_ED25519, getSeed().mSeed);
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
    assert(mKeyType == KEY_TYPE_ED25519);

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
    assert(sk.mKeyType == KEY_TYPE_ED25519);
    if (crypto_sign_keypair(pk.ed25519().data(), sk.mSecretKey.data()) != 0)
    {
        throw std::runtime_error("error generating random secret key");
    }
    return sk;
}

SecretKey
SecretKey::fromSeed(ByteSlice const& seed)
{
    PublicKey pk;
    SecretKey sk;
    assert(sk.mKeyType == KEY_TYPE_ED25519);

    if (seed.size() != crypto_sign_SEEDBYTES)
    {
        throw std::runtime_error("seed does not match byte size");
    }
    if (crypto_sign_seed_keypair(pk.ed25519().data(), sk.mSecretKey.data(),
                                 seed.data()) != 0)
    {
        throw std::runtime_error("error generating secret key from seed");
    }
    return sk;
}

SecretKey
SecretKey::fromStrKeySeed(std::string const& strKeySeed)
{
    uint8_t ver;
    std::vector<uint8_t> seed;
    if (!strKey::fromStrKey(strKeySeed, ver, seed) ||
        (ver != strKey::STRKEY_SEED_ED25519) ||
        (seed.size() != crypto_sign_SEEDBYTES) ||
        (strKeySeed.size() != strKey::getStrKeySize(crypto_sign_SEEDBYTES)))
    {
        throw std::runtime_error("invalid seed");
    }

    PublicKey pk;
    SecretKey sk;
    assert(sk.mKeyType == KEY_TYPE_ED25519);
    if (crypto_sign_seed_keypair(pk.ed25519().data(), sk.mSecretKey.data(),
                                 seed.data()) != 0)
    {
        throw std::runtime_error("error generating secret key from seed");
    }
    return sk;
}

SecretKey
SecretKey::fromBase58Seed(std::string const& base58Seed)
{
    auto pair = fromBase58Check(base58Seed);
    if (pair.first != B58_SEED_ED25519)
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
    assert(sk.mKeyType == KEY_TYPE_ED25519);
    if (crypto_sign_seed_keypair(pk.ed25519().data(), sk.mSecretKey.data(),
                                 pair.second.data()) != 0)
    {
        throw std::runtime_error("error generating secret key from seed");
    }
    return sk;
}

void
PubKeyUtils::clearVerifySigCache()
{
    std::lock_guard<std::mutex> guard(gVerifySigCacheMutex);
    gVerifySigCache.clear();
}

bool
PubKeyUtils::verifySig(PublicKey const& key, Signature const& signature,
                       ByteSlice const& bin)
{
    bool shouldCache = shouldCacheVerifySig(key, signature, bin);
    std::string cacheKey;

    if (shouldCache)
    {
        cacheKey = verifySigCacheKey(key, signature, bin);
        std::lock_guard<std::mutex> guard(gVerifySigCacheMutex);
        if (gVerifySigCache.exists(cacheKey))
        {
            return gVerifySigCache.get(cacheKey);
        }
    }

    bool ok = (crypto_sign_verify_detached(signature.data(), bin.data(), bin.size(),
                                           key.ed25519().data()) == 0);
    if (shouldCache)
    {
        std::lock_guard<std::mutex> guard(gVerifySigCacheMutex);
        gVerifySigCache.put(cacheKey, ok);
    }
    return ok;
}

std::string
PubKeyUtils::toShortString(PublicKey const& pk)
{
    return hexAbbrev(pk.ed25519());
}

std::string
PubKeyUtils::toStrKey(PublicKey const& pk)
{
    return strKey::toStrKey(strKey::STRKEY_PUBKEY_ED25519, pk.ed25519());
}

PublicKey
PubKeyUtils::fromStrKey(std::string const& s)
{
    PublicKey pk;
    uint8_t ver;
    std::vector<uint8_t> k;
    if (!strKey::fromStrKey(s, ver, k) ||
        (ver != strKey::STRKEY_PUBKEY_ED25519) ||
        (k.size() != crypto_sign_PUBLICKEYBYTES) ||
        (s.size() != strKey::getStrKeySize(crypto_sign_PUBLICKEYBYTES)))
    {
        throw std::runtime_error("bad public key");
    }
    std::copy(k.begin(), k.end(), pk.ed25519().begin());
    return pk;
}

std::string
PubKeyUtils::toBase58(PublicKey const& pk)
{
    // uses B58_PUBKEY_ED25519 prefix for ed25519
    return toBase58Check(B58_PUBKEY_ED25519, pk.ed25519());
}

PublicKey
PubKeyUtils::fromBase58(std::string const& s)
{
    PublicKey pk;
    pk.ed25519() = fromBase58Check256(B58_PUBKEY_ED25519, s);
    return pk;
}

SignatureHint
PubKeyUtils::getHint(PublicKey const& pk)
{
    SignatureHint res;
    memcpy(res.data(), &pk.ed25519().back() - res.size() + 1, res.size());
    return res;
}

bool
PubKeyUtils::hasHint(PublicKey const& pk, SignatureHint const& hint)
{
    return memcmp(&pk.ed25519().back() - hint.size() + 1, hint.data(),
                  hint.size()) == 0;
}

static void
logPublicKey(std::ostream& s, PublicKey const& pk)
{
    s << "PublicKey:" << std::endl
      << "  strKey: " << PubKeyUtils::toStrKey(pk) << std::endl
      << "  base58: " << PubKeyUtils::toBase58(pk) << std::endl
      << "  hex: " << binToHex(pk.ed25519()) << std::endl;
}

static void
logSecretKey(std::ostream& s, SecretKey const& sk)
{
    s << "Seed:" << std::endl
      << "  strKey: " << sk.getStrKeySeed() << std::endl
      << "  base58: " << sk.getBase58Seed() << std::endl;
    logPublicKey(s, sk.getPublicKey());
}

void
StrKeyUtils::logKey(std::ostream& s, std::string const& key)
{
    // if it's a hex string, display it in all forms
    try
    {
        uint256 data = hexToBin256(key);
        PublicKey pk;
        pk.type(KEY_TYPE_ED25519);
        pk.ed25519() = data;
        logPublicKey(s, pk);

        SecretKey sk(SecretKey::fromSeed(data));
        logSecretKey(s, sk);
        return;
    }
    catch (...)
    {
    }

    // see if it's a public key
    try
    {
        PublicKey pk = PubKeyUtils::fromStrKey(key);
        logPublicKey(s, pk);
        return;
    }
    catch (...)
    {
    }
    try
    {
        PublicKey pk = PubKeyUtils::fromBase58(key);
        logPublicKey(s, pk);
        return;
    }
    catch (...)
    {
    }

    // see if it's a seed
    try
    {
        SecretKey sk = SecretKey::fromStrKeySeed(key);
        logSecretKey(s, sk);
        return;
    }
    catch (...)
    {
    }
    try
    {
        SecretKey sk = SecretKey::fromBase58Seed(key);
        logSecretKey(s, sk);
        return;
    }
    catch (...)
    {
    }
    s << "Unknown key type" << std::endl;
}

Hash
HashUtils::random()
{
    Hash res;
    randombytes_buf(res.data(), res.size());
    return res;
}
}
