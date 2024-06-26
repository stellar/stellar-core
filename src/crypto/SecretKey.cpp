// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SecretKey.h"
#include "crypto/BLAKE2.h"
#include "crypto/CryptoError.h"
#include "crypto/Curve25519.h"
#include "crypto/Hex.h"
#include "crypto/KeyUtils.h"
#include "crypto/Random.h"
#include "crypto/StrKey.h"
#include "main/Config.h"
#include "transactions/SignatureUtils.h"
#include "util/GlobalChecks.h"
#include "util/HashOfHash.h"
#include "util/Math.h"
#include "util/RandomEvictionCache.h"
#include <Tracy.hpp>
#include <chrono>
#include <memory>
#include <mutex>
#include <sodium.h>
#include <type_traits>

#ifdef MSAN_ENABLED
#include <sanitizer/msan_interface.h>
#endif

#ifdef BUILD_TESTS
#include "lib/catch.hpp"
#endif

namespace stellar
{

// Process-wide global Ed25519 signature-verification cache.
//
// This is a pure mathematical function and has no relationship
// to the state of the process; caching its results centrally
// makes all signature-verification in the program faster and
// has no effect on correctness.

static std::mutex gVerifySigCacheMutex;
static RandomEvictionCache<Hash, bool> gVerifySigCache(0xffff,
                                                       /* separatePRNG */ true);
static uint64_t gVerifyCacheHit = 0;
static uint64_t gVerifyCacheMiss = 0;

static Hash
verifySigCacheKey(PublicKey const& key, Signature const& signature,
                  ByteSlice const& bin)
{
    releaseAssert(key.type() == PUBLIC_KEY_TYPE_ED25519);

    BLAKE2 hasher;
    hasher.add(key.ed25519());
    hasher.add(signature);
    hasher.add(bin);
    return hasher.finish();
}

SecretKey::SecretKey() : mKeyType(PUBLIC_KEY_TYPE_ED25519)
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

PublicKey const&
SecretKey::getPublicKey() const
{
    return mPublicKey;
}

SecretKey::Seed
SecretKey::getSeed() const
{
    releaseAssert(mKeyType == PUBLIC_KEY_TYPE_ED25519);

    Seed seed;
    seed.mKeyType = mKeyType;
    if (crypto_sign_ed25519_sk_to_seed(seed.mSeed.data(), mSecretKey.data()) !=
        0)
    {
        throw CryptoError("error extracting seed from secret key");
    }
    return seed;
}

SecretValue
SecretKey::getStrKeySeed() const
{
    releaseAssert(mKeyType == PUBLIC_KEY_TYPE_ED25519);

    return strKey::toStrKey(strKey::STRKEY_SEED_ED25519, getSeed().mSeed);
}

std::string
SecretKey::getStrKeyPublic() const
{
    return KeyUtils::toStrKey(getPublicKey());
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
    ZoneScoped;
    releaseAssert(mKeyType == PUBLIC_KEY_TYPE_ED25519);

    Signature out(crypto_sign_BYTES, 0);
    if (crypto_sign_detached(out.data(), NULL, bin.data(), bin.size(),
                             mSecretKey.data()) != 0)
    {
        throw CryptoError("error while signing");
    }
    return out;
}

SecretKey
SecretKey::random()
{
    SecretKey sk;
    releaseAssert(sk.mKeyType == PUBLIC_KEY_TYPE_ED25519);
    if (crypto_sign_keypair(sk.mPublicKey.ed25519().data(),
                            sk.mSecretKey.data()) != 0)
    {
        throw CryptoError("error generating random secret key");
    }
#ifdef MSAN_ENABLED
    __msan_unpoison(out.key.data(), out.key.size());
#endif
    return sk;
}

struct SignVerifyTestcase
{
    SecretKey key;
    std::vector<uint8_t> msg;
    Signature sig;
    void
    sign()
    {
        sig = key.sign(msg);
    }
    void
    verify()
    {
        if (!PubKeyUtils::verifySig(key.getPublicKey(), sig, msg))
        {
            throw std::runtime_error("verify failed");
        }
    }
    static SignVerifyTestcase
    create()
    {
        SignVerifyTestcase st;
        st.key = SecretKey::random();
        st.msg = randomBytes(256);
        return st;
    }
};

void
SecretKey::benchmarkOpsPerSecond(size_t& sign, size_t& verify,
                                 size_t iterations, size_t cachedVerifyPasses)
{
    namespace ch = std::chrono;
    using clock = ch::high_resolution_clock;
    using usec = ch::microseconds;

    std::vector<SignVerifyTestcase> cases;

    for (size_t i = 0; i < iterations; ++i)
    {
        cases.push_back(SignVerifyTestcase::create());
    }

    auto signStart = clock::now();
    for (auto& c : cases)
    {
        c.sign();
    }
    auto signEnd = clock::now();
    auto verifyStart = clock::now();
    for (auto pass = 0; pass < cachedVerifyPasses; ++pass)
    {
        if (pass == 1)
        {
            // If we have more than 1 pass, reset clock after
            // first so we are only measuring cache-hits.
            verifyStart = clock::now();
        }
        for (auto& c : cases)
        {
            c.verify();
        }
    }
    auto verifyEnd = clock::now();

    auto signUsec = ch::duration_cast<usec>(signEnd - signStart);
    auto verifyUsec = ch::duration_cast<usec>(verifyEnd - verifyStart);
    sign = 1000000 / std::max(size_t(1), size_t(signUsec.count() / iterations));
    verify =
        1000000 / std::max(size_t(1), size_t(verifyUsec.count() / iterations));
}

#ifdef BUILD_TESTS
template <typename Rng>
static std::vector<uint8_t>
getPRNGBytes(size_t n, Rng& engine)
{
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < n; ++i)
    {
        bytes.push_back(static_cast<uint8_t>(engine()));
    }
    return bytes;
}

template <typename Rng>
static SecretKey
pseudoRandomForTestingFromPRNG(Rng& engine)
{
    return SecretKey::fromSeed(getPRNGBytes(crypto_sign_SEEDBYTES, engine));
}

SecretKey
SecretKey::pseudoRandomForTesting()
{
    // Reminder: this is not cryptographic randomness or even particularly hard
    // to guess PRNG-ness. It's intended for _deterministic_ use, when you want
    // "slightly random-ish" keys, for test-data generation.
    return pseudoRandomForTestingFromPRNG(Catch::rng());
}

SecretKey
SecretKey::pseudoRandomForTestingFromSeed(unsigned int seed)
{
    // Reminder: this is not cryptographic randomness or even particularly hard
    // to guess PRNG-ness. It's intended for _deterministic_ use, when you want
    // "slightly random-ish" keys, for test-data generation.
    stellar_default_random_engine tmpEngine(seed);
    return pseudoRandomForTestingFromPRNG(tmpEngine);
}
#endif

SecretKey
SecretKey::fromSeed(ByteSlice const& seed)
{
    SecretKey sk;
    releaseAssert(sk.mKeyType == PUBLIC_KEY_TYPE_ED25519);

    if (seed.size() != crypto_sign_SEEDBYTES)
    {
        throw CryptoError("seed does not match byte size");
    }
    if (crypto_sign_seed_keypair(sk.mPublicKey.ed25519().data(),
                                 sk.mSecretKey.data(), seed.data()) != 0)
    {
        throw CryptoError("error generating secret key from seed");
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
        throw CryptoError("invalid seed");
    }

    SecretKey sk;
    releaseAssert(sk.mKeyType == PUBLIC_KEY_TYPE_ED25519);
    if (crypto_sign_seed_keypair(sk.mPublicKey.ed25519().data(),
                                 sk.mSecretKey.data(), seed.data()) != 0)
    {
        throw CryptoError("error generating secret key from seed");
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
PubKeyUtils::maybeSeedVerifySigCache(unsigned int seed)
{
    std::lock_guard<std::mutex> guard(gVerifySigCacheMutex);
    gVerifySigCache.maybeSeed(seed);
}

void
PubKeyUtils::flushVerifySigCacheCounts(uint64_t& hits, uint64_t& misses)
{
    std::lock_guard<std::mutex> guard(gVerifySigCacheMutex);
    hits = gVerifyCacheHit;
    misses = gVerifyCacheMiss;
    gVerifyCacheHit = 0;
    gVerifyCacheMiss = 0;
}

std::string
KeyFunctions<PublicKey>::getKeyTypeName()
{
    return "public key";
}

bool
KeyFunctions<PublicKey>::getKeyVersionIsSupported(
    strKey::StrKeyVersionByte keyVersion)
{
    switch (keyVersion)
    {
    case strKey::STRKEY_PUBKEY_ED25519:
        return true;
    default:
        return false;
    }
}

bool
KeyFunctions<PublicKey>::getKeyVersionIsVariableLength(
    strKey::StrKeyVersionByte keyVersion)
{
    return false;
}

PublicKeyType
KeyFunctions<PublicKey>::toKeyType(strKey::StrKeyVersionByte keyVersion)
{
    switch (keyVersion)
    {
    case strKey::STRKEY_PUBKEY_ED25519:
        return PublicKeyType::PUBLIC_KEY_TYPE_ED25519;
    default:
        throw CryptoError("invalid public key type");
    }
}

strKey::StrKeyVersionByte
KeyFunctions<PublicKey>::toKeyVersion(PublicKeyType keyType)
{
    switch (keyType)
    {
    case PublicKeyType::PUBLIC_KEY_TYPE_ED25519:
        return strKey::STRKEY_PUBKEY_ED25519;
    default:
        throw CryptoError("invalid public key type");
    }
}

uint256&
KeyFunctions<PublicKey>::getEd25519Value(PublicKey& key)
{
    switch (key.type())
    {
    case PUBLIC_KEY_TYPE_ED25519:
        return key.ed25519();
    default:
        throw CryptoError("invalid public key type");
    }
}

uint256 const&
KeyFunctions<PublicKey>::getEd25519Value(PublicKey const& key)
{
    switch (key.type())
    {
    case PUBLIC_KEY_TYPE_ED25519:
        return key.ed25519();
    default:
        throw CryptoError("invalid public key type");
    }
}

std::vector<uint8_t>
KeyFunctions<PublicKey>::getKeyValue(PublicKey const& key)
{
    return xdr::xdr_to_opaque(getEd25519Value(key));
}

void
KeyFunctions<PublicKey>::setKeyValue(PublicKey& key,
                                     std::vector<uint8_t> const& data)
{
    switch (key.type())
    {
    case PUBLIC_KEY_TYPE_ED25519:
        xdr::xdr_from_opaque(data, key.ed25519());
        break;
    default:
        throw CryptoError("invalid public key type");
    }
}

bool
PubKeyUtils::verifySig(PublicKey const& key, Signature const& signature,
                       ByteSlice const& bin)
{
    ZoneScoped;
    releaseAssert(key.type() == PUBLIC_KEY_TYPE_ED25519);
    if (signature.size() != 64)
    {
        return false;
    }

    auto cacheKey = verifySigCacheKey(key, signature, bin);

    {
        std::lock_guard<std::mutex> guard(gVerifySigCacheMutex);
        if (gVerifySigCache.exists(cacheKey))
        {
            ++gVerifyCacheHit;
            std::string hitStr("hit");
            ZoneText(hitStr.c_str(), hitStr.size());
            return gVerifySigCache.get(cacheKey);
        }
    }

    std::string missStr("miss");
    ZoneText(missStr.c_str(), missStr.size());
    bool ok =
        (crypto_sign_verify_detached(signature.data(), bin.data(), bin.size(),
                                     key.ed25519().data()) == 0);
    std::lock_guard<std::mutex> guard(gVerifySigCacheMutex);
    ++gVerifyCacheMiss;
    gVerifySigCache.put(cacheKey, ok);
    return ok;
}

PublicKey
PubKeyUtils::random()
{
    PublicKey pk;
    pk.type(PUBLIC_KEY_TYPE_ED25519);
    pk.ed25519().resize(crypto_sign_PUBLICKEYBYTES);
    randombytes_buf(pk.ed25519().data(), pk.ed25519().size());
    return pk;
}

#ifdef BUILD_TESTS
PublicKey
PubKeyUtils::pseudoRandomForTesting()
{
    return SecretKey::pseudoRandomForTesting().getPublicKey();
}
#endif

static void
logPublicKey(std::ostream& s, PublicKey const& pk)
{
    s << "PublicKey:" << std::endl
      << "  strKey: " << KeyUtils::toStrKey(pk) << std::endl
      << "  hex: " << binToHex(pk.ed25519()) << std::endl;
}

static void
logSecretKey(std::ostream& s, SecretKey const& sk)
{
    s << "Seed:" << std::endl
      << "  strKey: " << sk.getStrKeySeed().value << std::endl;
    logPublicKey(s, sk.getPublicKey());
}

void
StrKeyUtils::logKey(std::ostream& s, std::string const& key)
{

    // see if it's a public key
    try
    {
        PublicKey pk = KeyUtils::fromStrKey<PublicKey>(key);
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

    // if it's a hex string, display it in all forms
    try
    {
        uint256 data = hexToBin256(key);
        PublicKey pk;
        pk.type(PUBLIC_KEY_TYPE_ED25519);
        pk.ed25519() = data;
        s << "Interpreted as ";
        logPublicKey(s, pk);

        s << std::endl;
        SecretKey sk(SecretKey::fromSeed(data));
        s << "Interpreted as ";
        logSecretKey(s, sk);

        s << std::endl;
        s << "Other interpretations:" << std::endl;
        s << "  STRKEY_PRE_AUTH_TX: "
          << strKey::toStrKey(strKey::STRKEY_PRE_AUTH_TX, data).value
          << std::endl;
        s << "  STRKEY_HASH_X: "
          << strKey::toStrKey(strKey::STRKEY_HASH_X, data).value << std::endl;
        s << "  STRKEY_SIGNED_PAYLOAD: "
          << strKey::toStrKey(strKey::STRKEY_SIGNED_PAYLOAD_ED25519, data).value
          << std::endl;
        s << "  STRKEY_MUXED_ACCOUNT_ED25519: "
          << strKey::toStrKey(strKey::STRKEY_MUXED_ACCOUNT_ED25519, data).value
          << std::endl;
        s << "  STRKEY_CONTRACT: "
          << strKey::toStrKey(strKey::STRKEY_CONTRACT, data).value << std::endl;
        return;
    }
    catch (...)
    {
    }

    // Try generic strkey decoding for other strkey types

    uint8_t outVersion;
    std::vector<uint8_t> decoded;
    if (strKey::fromStrKey(key, outVersion, decoded))
    {
        s << "StrKey:" << std::endl;
        switch (outVersion)
        {
        case strKey::STRKEY_PUBKEY_ED25519:
            s << "  type: STRKEY_PUBKEY_ED25519" << std::endl;
            break;
        case strKey::STRKEY_SIGNED_PAYLOAD_ED25519:
            s << "  type: STRKEY_SIGNED_PAYLOAD_ED25519" << std::endl;
            break;
        case strKey::STRKEY_SEED_ED25519:
            s << "  type: STRKEY_SEED_ED25519" << std::endl;
            break;
        case strKey::STRKEY_PRE_AUTH_TX:
            s << "  type: STRKEY_PRE_AUTH_TX" << std::endl;
            break;
        case strKey::STRKEY_HASH_X:
            s << "  type: STRKEY_HASH_X" << std::endl;
            break;
        case strKey::STRKEY_MUXED_ACCOUNT_ED25519:
            throw std::runtime_error(
                "unexpected StrKey type STRKEY_MUXED_ACCOUNT_ED25519");
            break;
        case strKey::STRKEY_CONTRACT:
            s << "  type: STRKEY_CONTRACT" << std::endl;
            break;
        default:
            s << "  type: unknown" << std::endl;
            break;
        }
        s << "  hex: " << binToHex(decoded) << std::endl;
        return;
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

#ifdef BUILD_TESTS
Hash
HashUtils::pseudoRandomForTesting()
{
    Hash res;
    auto bytes = getPRNGBytes(res.size(), gRandomEngine);
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        res[i] = bytes[i];
    }
    return res;
}
#endif
}

namespace std
{
size_t
hash<stellar::PublicKey>::operator()(stellar::PublicKey const& k) const noexcept
{
    using namespace stellar;
    releaseAssert(k.type() == stellar::PUBLIC_KEY_TYPE_ED25519);

    return std::hash<stellar::uint256>()(k.ed25519());
}
}
