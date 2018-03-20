#pragma once

#include "crypto/ECDH.h"
#include "overlay/Peer.h"
#include "overlay/PeerSharedKeyId.h"
#include "util/lrucache.hpp"
#include "xdr/Stellar-types.h"

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

namespace stellar
{
class PeerAuth
{
    // Authentication system keys. Our ECDH secret and public keys are
    // randomized on startup. We send the public half to each connecting
    // node, signed with our long-lived private node key, and then do
    // HKDF_extract(ECDH(A_sec,B_pub) || A_pub || B_pub) to derive a
    // medium-duration MAC key for each possible remote peer.
    //
    // Then each peer _session_ gets two sub-MAC-keys derived by
    // HKDF_expand(K{us,them}, 0 || nonce_A || nonce_B) and
    // HKDF_expand(K{us,them}, 1 || nonce_B || nonce_A) for
    // use in a particular A-called-B p2p session.

    Application& mApp;
    Curve25519Secret mECDHSecretKey;
    Curve25519Public mECDHPublicKey;
    AuthCert mCert;

    cache::lru_cache<PeerSharedKeyId, HmacSha256Key> mSharedKeyCache;

    HmacSha256Key getSharedKey(Curve25519Public const& remotePublic,
                               Peer::PeerRole role);

  public:
    PeerAuth(Application& app);

    AuthCert getAuthCert();
    bool verifyRemoteAuthCert(NodeID const& remoteNode, AuthCert const& cert);

    HmacSha256Key getSendingMacKey(Curve25519Public const& remotePublic,
                                   uint256 const& localNonce,
                                   uint256 const& remoteNonce,
                                   Peer::PeerRole role);
    HmacSha256Key getReceivingMacKey(Curve25519Public const& remotePublic,
                                     uint256 const& localNonce,
                                     uint256 const& remoteNonce,
                                     Peer::PeerRole role);
};
}
