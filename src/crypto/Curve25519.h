#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ByteSlice.h"
#include "util/Logging.h"
#include "xdr/Stellar-types.h"
#include <fmt/format.h>
#include <functional>
#include <sodium.h>

namespace stellar
{
// This module contains functions for doing ECDH on Curve25519. Despite the
// equivalence between this curve and Ed25519 (used in signatures, see
// SecretKey.h) we use Curve25519 keys _only_ for ECDH shared-key-agreement
// between peers, as part of the p2p overlay system (see
// overlay/PeerAuth.h), not the transaction-signing or SCP-message-signing
// systems. Signatures use Ed25519 keys.
//
// Do not mix keys between these subsystems: i.e. do not convert a signing
// Ed25519 key to an ECDH Curve25519 key. It's possible to do but
// complicates, and potentially undermines, the security analysis of each.
// We prefer to use random-per-session Curve25519 keys, and merely sign
// them with the long-lived Ed25519 keys during p2p handshake.

// Read a scalar from /dev/urandom.
Curve25519Secret curve25519RandomSecret();

// Calculate a public Curve25519 point from a private scalar.
Curve25519Public curve25519DerivePublic(Curve25519Secret const& sec);

// clears the keys by running sodium_memzero
void clearCurve25519Keys(Curve25519Public& localPublic,
                         Curve25519Secret& localSecret);

// Calculate HKDF_extract(localSecret * remotePublic || publicA || publicB)
//
// Where
//   publicA = localFirst ? localPublic : remotePublic
//   publicB = localFirst ? remotePublic : localPublic

HmacSha256Key curve25519DeriveSharedKey(Curve25519Secret const& localSecret,
                                        Curve25519Public const& localPublic,
                                        Curve25519Public const& remotePublic,
                                        bool localFirst);

xdr::opaque_vec<> curve25519Decrypt(Curve25519Secret const& localSecret,
                                    Curve25519Public const& localPublic,
                                    ByteSlice const& encrypted);

template <uint32_t N>
xdr::opaque_vec<N>
curve25519Encrypt(Curve25519Public const& remotePublic, ByteSlice const& bin)
{
    const uint64_t CIPHERTEXT_LEN = crypto_box_SEALBYTES + bin.size();
    if (CIPHERTEXT_LEN > N)
    {
        throw std::runtime_error(fmt::format(
            "CIPHERTEXT_LEN({}) is greater than N({})", CIPHERTEXT_LEN, N));
    }

    xdr::opaque_vec<N> ciphertext(CIPHERTEXT_LEN, 0);

    if (crypto_box_seal(ciphertext.data(), bin.data(), bin.size(),
                        remotePublic.key.data()) != 0)
    {
        throw std::runtime_error("curve25519Encrypt failed");
    }

    return ciphertext;
}
}

namespace std
{
template <> struct hash<stellar::Curve25519Public>
{
    size_t operator()(stellar::Curve25519Public const& x) const noexcept;
};
}
