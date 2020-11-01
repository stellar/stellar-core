// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/Curve25519.h"
#include "crypto/CryptoError.h"
#include "crypto/SHA.h"
#include "util/HashOfHash.h"
#include <Tracy.hpp>
#include <functional>

#ifdef MSAN_ENABLED
#include <sanitizer/msan_interface.h>
#endif

namespace stellar
{

Curve25519Secret
curve25519RandomSecret()
{
    Curve25519Secret out;
    randombytes_buf(out.key.data(), out.key.size());
#ifdef MSAN_ENABLED
    __msan_unpoison(out.key.data(), out.key.size());
#endif
    return out;
}

Curve25519Public
curve25519DerivePublic(Curve25519Secret const& sec)
{
    ZoneScoped;
    Curve25519Public out;
    if (crypto_scalarmult_base(out.key.data(), sec.key.data()) != 0)
    {
        throw CryptoError("Could not derive key (mult_base)");
    }
    return out;
}

void
clearCurve25519Keys(Curve25519Public& localPublic,
                    Curve25519Secret& localSecret)
{
    sodium_memzero(localPublic.key.data(), localPublic.key.size());
    sodium_memzero(localSecret.key.data(), localSecret.key.size());
}

HmacSha256Key
curve25519DeriveSharedKey(Curve25519Secret const& localSecret,
                          Curve25519Public const& localPublic,
                          Curve25519Public const& remotePublic, bool localFirst)
{
    ZoneScoped;
    auto const& publicA = localFirst ? localPublic : remotePublic;
    auto const& publicB = localFirst ? remotePublic : localPublic;

    unsigned char q[crypto_scalarmult_BYTES];
    if (crypto_scalarmult(q, localSecret.key.data(), remotePublic.key.data()) !=
        0)
    {
        throw CryptoError("Could not derive shared key (mult)");
    }
#ifdef MSAN_ENABLED
    __msan_unpoison(q, crypto_scalarmult_BYTES);
#endif
    std::vector<uint8_t> buf;
    buf.reserve(crypto_scalarmult_BYTES + publicA.key.size() +
                publicB.key.size());
    buf.insert(buf.end(), q, q + crypto_scalarmult_BYTES);
    buf.insert(buf.end(), publicA.key.begin(), publicA.key.end());
    buf.insert(buf.end(), publicB.key.begin(), publicB.key.end());
    return hkdfExtract(buf);
}

xdr::opaque_vec<>
curve25519Decrypt(Curve25519Secret const& localSecret,
                  Curve25519Public const& localPublic,
                  ByteSlice const& encrypted)
{
    ZoneScoped;
    if (encrypted.size() < crypto_box_SEALBYTES)
    {
        throw CryptoError(
            "encrypted.size() is less than crypto_box_SEALBYTES!");
    }

    const uint64_t MESSAGE_LEN = encrypted.size() - crypto_box_SEALBYTES;
    xdr::opaque_vec<> decrypted(MESSAGE_LEN, 0);

    if (crypto_box_seal_open(decrypted.data(), encrypted.data(),
                             encrypted.size(), localPublic.key.data(),
                             localSecret.key.data()) != 0)
    {
        throw CryptoError("curve25519Decrypt failed");
    }

    return decrypted;
}
}

namespace std
{
size_t
hash<stellar::Curve25519Public>::
operator()(stellar::Curve25519Public const& k) const noexcept
{
    return std::hash<stellar::uint256>()(k.key);
}
}
