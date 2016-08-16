// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SecretKey.h"
#include "crypto/StrKey.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include <sodium.h>
#include <type_traits>
#include <memory>
#include "util/make_unique.h"
#include "util/HashOfHash.h"
#include <mutex>
#include "main/Config.h"
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
static cache::lru_cache<Hash, bool> gVerifySigCache(0xffff);
static std::unique_ptr<SHA256> gHasher = SHA256::create();
static uint64_t gVerifyCacheHit = 0;
static uint64_t gVerifyCacheMiss = 0;
static uint64_t gVerifyCacheIgnore = 0;

static bool
shouldCacheVerifySig(PublicKey const& key, Signature const& signature,
                     ByteSlice const& bin)
{
    return true;
}

static Hash
verifySigCacheKey(PublicKey const& key, Signature const& signature,
                  ByteSlice const& bin)
{
    gHasher->reset();
    gHasher->add(key.ed25519());
    gHasher->add(signature);
    gHasher->add(bin);
    return gHasher->finish();
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

SecretKey::~SecretKey()
{
    std::memset(mSecretKey.data(), 0, mSecretKey.size());
}

SecretKey::Seed::~Seed()
{
    std::memset(mSeed.data(), 0, mSeed.size());
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

void
PubKeyUtils::clearVerifySigCache()
{
    std::lock_guard<std::mutex> guard(gVerifySigCacheMutex);
    gVerifySigCache.clear();
}

void
PubKeyUtils::flushVerifySigCacheCounts(uint64_t& hits, uint64_t& misses,
                                       uint64_t& ignores)
{
    std::lock_guard<std::mutex> guard(gVerifySigCacheMutex);
    hits = gVerifyCacheHit;
    misses = gVerifyCacheMiss;
    ignores = gVerifyCacheIgnore;
    gVerifyCacheHit = 0;
    gVerifyCacheMiss = 0;
    gVerifyCacheIgnore = 0;
}

bool
PubKeyUtils::verifySig(PublicKey const& key, Signature const& signature,
                       ByteSlice const& bin)
{
    bool shouldCache = shouldCacheVerifySig(key, signature, bin);
    Hash cacheKey;

    if (shouldCache)
    {
        cacheKey = verifySigCacheKey(key, signature, bin);
        std::lock_guard<std::mutex> guard(gVerifySigCacheMutex);
        if (gVerifySigCache.exists(cacheKey))
        {
            ++gVerifyCacheHit;
            return gVerifySigCache.get(cacheKey);
        }
        ++gVerifyCacheMiss;
    }
    else
    {
        ++gVerifyCacheIgnore;
    }

    bool ok =
        (crypto_sign_verify_detached(signature.data(), bin.data(), bin.size(),
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
    return toStrKey(pk).substr(0, 5);
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
        throw std::invalid_argument("bad public key");
    }
    std::copy(k.begin(), k.end(), pk.ed25519().begin());
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

PublicKey
PubKeyUtils::random()
{
    PublicKey pk;
    pk.type(KEY_TYPE_ED25519);
    pk.ed25519().resize(crypto_sign_PUBLICKEYBYTES);
    randombytes_buf(pk.ed25519().data(), pk.ed25519().size());
    return pk;
}

static void
logPublicKey(std::ostream& s, PublicKey const& pk)
{
    s << "PublicKey:" << std::endl
      << "  strKey: " << PubKeyUtils::toStrKey(pk) << std::endl
      << "  hex: " << binToHex(pk.ed25519()) << std::endl;
}

static void
logSecretKey(std::ostream& s, SecretKey const& sk)
{
    s << "Seed:" << std::endl
      << "  strKey: " << sk.getStrKeySeed() << std::endl;
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

namespace std
{
size_t hash<stellar::PublicKey>::operator()(stellar::PublicKey const& k) const
    noexcept
{
    return std::hash<stellar::uint256>()(k.ed25519());
}
}
