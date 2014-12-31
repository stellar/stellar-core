// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "crypto/Sign.h"
#include "crypto/Base58.h"
#include "crypto/ByteSlice.h"
#include <sodium.h>
#include <type_traits>

namespace stellar
{

static_assert(crypto_sign_PUBLICKEYBYTES == sizeof(uint256), "Unexpected public key length");
static_assert(crypto_sign_SECRETKEYBYTES == sizeof(uint512), "Unexpected secret key length");
static_assert(crypto_sign_BYTES == sizeof(uint512), "Unexpected signature length");

bool
PublicKey::verify(uint512 const& signature, ByteSlice const& bin) const
{
    return crypto_sign_verify_detached(signature.data(),
                                       bin.data(),
                                       bin.size(),
                                       data()) == 0;
}

SecretKey::SecretKey()
{
}

PublicKey
SecretKey::getPublicKey() const
{
    PublicKey pk;
    if (crypto_sign_ed25519_sk_to_pk(pk.data(), data()) != 0)
        throw std::runtime_error("error extracting public key from secret key");
    return pk;
}

std::string
SecretKey::getBase58Seed() const
{
    uint256 seed;
    if (crypto_sign_ed25519_sk_to_seed(seed.data(), data()) != 0)
        throw std::runtime_error("error extracting seed from secret key");
    return toBase58Check(VER_SEED, seed);
}

uint512
SecretKey::sign(ByteSlice const& bin) const
{
    uint512 out;
    if (crypto_sign_detached(out.data(), NULL,
                             bin.data(), bin.size(), data()) != 0)
        throw std::runtime_error("error while signing");
    return out;
}

SecretKey
SecretKey::random()
{
    PublicKey pk;
    SecretKey sk;
    if (crypto_sign_keypair(pk.data(), sk.data()) != 0)
        throw std::runtime_error("error generating random secret key");
    return sk;
}

SecretKey
SecretKey::fromBase58Seed(std::string const& base58Seed)
{
    auto pair = fromBase58Check(base58Seed);
    if (pair.first != VER_SEED)
        throw std::runtime_error("bad version byte on secret key base58 seed");
    if (pair.second.size() != crypto_sign_SEEDBYTES)
        throw std::runtime_error("wrong base58 seed length for secret key");

    PublicKey pk;
    SecretKey sk;
    if (crypto_sign_seed_keypair(pk.data(), sk.data(),
                                 pair.second.data()) != 0)
        throw std::runtime_error("error generating secret key from seed");
    return sk;
}

}
