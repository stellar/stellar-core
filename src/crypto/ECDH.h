#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "xdr/Stellar-types.h"
#include <functional>

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
Curve25519Secret EcdhRandomSecret();

// Calculate a public Curve25519 point from a private scalar.
Curve25519Public EcdhDerivePublic(Curve25519Secret const& sec);

// Calculate HKDF_extract(localSecret * remotePublic || publicA || publicB)
//
// Where
//   publicA = localFirst ? localPublic : remotePublic
//   publicB = localFirst ? remotePublic : localPublic

HmacSha256Key EcdhDeriveSharedKey(Curve25519Secret const& localSecret,
                                  Curve25519Public const& localPublic,
                                  Curve25519Public const& remotePublic,
                                  bool localFirst);
}

namespace std
{
template <> struct hash<stellar::Curve25519Public>
{
    size_t operator()(stellar::Curve25519Public const& x) const noexcept;
};
}
