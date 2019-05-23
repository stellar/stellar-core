// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SecretKey.h"
#include "crypto/Hex.h"
#include "crypto/StrKey.h"
#include "util/HashOfHash.h"
#include "util/Math.h"
#include <sodium.h>

#ifdef MSAN_ENABLED
#include <sanitizer/msan_interface.h>
#endif

namespace stellar
{

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
    assert(mKeyType == PUBLIC_KEY_TYPE_ED25519);

    Seed seed;
    seed.mKeyType = mKeyType;
    if (crypto_sign_ed25519_sk_to_seed(seed.mSeed.data(), mSecretKey.data()) !=
        0)
    {
        throw std::runtime_error("error extracting seed from secret key");
    }
    return seed;
}

SecretValue
SecretKey::getStrKeySeed() const
{
    assert(mKeyType == PUBLIC_KEY_TYPE_ED25519);

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
    assert(mKeyType == PUBLIC_KEY_TYPE_ED25519);

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
    SecretKey sk;
    assert(sk.mKeyType == PUBLIC_KEY_TYPE_ED25519);
    if (crypto_sign_keypair(sk.mPublicKey.ed25519().data(),
                            sk.mSecretKey.data()) != 0)
    {
        throw std::runtime_error("error generating random secret key");
    }
#ifdef MSAN_ENABLED
    __msan_unpoison(out.key.data(), out.key.size());
#endif
    return sk;
}

#ifdef BUILD_TESTS
static SecretKey
pseudoRandomForTestingFromPRNG(std::default_random_engine& engine)
{
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < crypto_sign_SEEDBYTES; ++i)
    {
        bytes.push_back(static_cast<uint8_t>(engine()));
    }
    return SecretKey::fromSeed(bytes);
}

SecretKey
SecretKey::pseudoRandomForTesting()
{
    // Reminder: this is not cryptographic randomness or even particularly hard
    // to guess PRNG-ness. It's intended for _deterministic_ use, when you want
    // "slightly random-ish" keys, for test-data generation.
    return pseudoRandomForTestingFromPRNG(gRandomEngine);
}

SecretKey
SecretKey::pseudoRandomForTestingFromSeed(unsigned int seed)
{
    // Reminder: this is not cryptographic randomness or even particularly hard
    // to guess PRNG-ness. It's intended for _deterministic_ use, when you want
    // "slightly random-ish" keys, for test-data generation.
    std::default_random_engine tmpEngine(seed);
    return pseudoRandomForTestingFromPRNG(tmpEngine);
}
#endif

SecretKey
SecretKey::fromSeed(ByteSlice const& seed)
{
    SecretKey sk;
    assert(sk.mKeyType == PUBLIC_KEY_TYPE_ED25519);

    if (seed.size() != crypto_sign_SEEDBYTES)
    {
        throw std::runtime_error("seed does not match byte size");
    }
    if (crypto_sign_seed_keypair(sk.mPublicKey.ed25519().data(),
                                 sk.mSecretKey.data(), seed.data()) != 0)
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

    SecretKey sk;
    assert(sk.mKeyType == PUBLIC_KEY_TYPE_ED25519);
    if (crypto_sign_seed_keypair(sk.mPublicKey.ed25519().data(),
                                 sk.mSecretKey.data(), seed.data()) != 0)
    {
        throw std::runtime_error("error generating secret key from seed");
    }
    return sk;
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

PublicKeyType
KeyFunctions<PublicKey>::toKeyType(strKey::StrKeyVersionByte keyVersion)
{
    switch (keyVersion)
    {
    case strKey::STRKEY_PUBKEY_ED25519:
        return PublicKeyType::PUBLIC_KEY_TYPE_ED25519;
    default:
        throw std::invalid_argument("invalid public key type");
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
        throw std::invalid_argument("invalid public key type");
    }
}

uint256&
KeyFunctions<PublicKey>::getKeyValue(PublicKey& key)
{
    switch (key.type())
    {
    case PUBLIC_KEY_TYPE_ED25519:
        return key.ed25519();
    default:
        throw std::invalid_argument("invalid public key type");
    }
}

uint256 const&
KeyFunctions<PublicKey>::getKeyValue(PublicKey const& key)
{
    switch (key.type())
    {
    case PUBLIC_KEY_TYPE_ED25519:
        return key.ed25519();
    default:
        throw std::invalid_argument("invalid public key type");
    }
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
    // if it's a hex string, display it in all forms
    try
    {
        uint256 data = hexToBin256(key);
        PublicKey pk;
        pk.type(PUBLIC_KEY_TYPE_ED25519);
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
size_t
hash<stellar::PublicKey>::operator()(stellar::PublicKey const& k) const noexcept
{
    assert(k.type() == stellar::PUBLIC_KEY_TYPE_ED25519);

    return std::hash<stellar::uint256>()(k.ed25519());
}
}
