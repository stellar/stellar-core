// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/ECDH.h"
#include "crypto/SHA.h"
#include "util/HashOfHash.h"
#include <functional>
#include <sodium.h>

namespace stellar
{

Curve25519Secret
EcdhRandomSecret()
{
    Curve25519Secret out;
    randombytes_buf(out.key.data(), out.key.size());
    return out;
}

Curve25519Public
EcdhDerivePublic(Curve25519Secret const& sec)
{
    Curve25519Public out;
    if (crypto_scalarmult_base(out.key.data(), sec.key.data()) != 0)
    {
        throw std::runtime_error("Could not derive key (mult_base)");
    }
    return out;
}

HmacSha256Key
EcdhDeriveSharedKey(Curve25519Secret const& localSecret,
                    Curve25519Public const& localPublic,
                    Curve25519Public const& remotePublic, bool localFirst)
{
    auto const& publicA = localFirst ? localPublic : remotePublic;
    auto const& publicB = localFirst ? remotePublic : localPublic;

    unsigned char q[crypto_scalarmult_BYTES];
    if (crypto_scalarmult(q, localSecret.key.data(), remotePublic.key.data()) !=
        0)
    {
        throw std::runtime_error("Could not derive shared key (mult)");
    }
    std::vector<uint8_t> buf(q, q + crypto_scalarmult_BYTES);
    buf.insert(buf.end(), publicA.key.begin(), publicA.key.end());
    buf.insert(buf.end(), publicB.key.begin(), publicB.key.end());
    return hkdfExtract(buf);
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
