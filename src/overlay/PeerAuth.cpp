// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/PeerAuth.h"
#include "crypto/ECDH.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "main/Application.h"
#include "main/Config.h"
#include "util/Logging.h"
#include "xdrpp/marshal.h"

namespace stellar
{

using xdr::operator==;

// Certs expire every hour, are reissued every half hour.
static const uint64_t expirationLimit = 3600;

static AuthCert
makeAuthCert(Application& app, Curve25519Public const& pub)
{
    AuthCert cert;
    // Certs are refreshed every half hour, good for an hour.
    cert.pubkey = pub;
    cert.expiration = app.timeNow() + expirationLimit;

    auto hash = sha256(xdr::xdr_to_opaque(
        app.getNetworkID(), ENVELOPE_TYPE_AUTH, cert.expiration, cert.pubkey));
    CLOG(DEBUG, "Overlay") << "PeerAuth signing cert hash: " << hexAbbrev(hash);
    cert.sig = app.getConfig().NODE_SEED.sign(hash);
    return cert;
}

PeerAuth::PeerAuth(Application& app)
    : mApp(app)
    , mECDHSecretKey(EcdhRandomSecret())
    , mECDHPublicKey(EcdhDerivePublic(mECDHSecretKey))
    , mCert(makeAuthCert(app, mECDHPublicKey))
    , mSharedKeyCache(0xffff)
{
}

AuthCert
PeerAuth::getAuthCert()
{
    if (mCert.expiration < mApp.timeNow() + (expirationLimit / 2))
    {
        mCert = makeAuthCert(mApp, mECDHPublicKey);
    }
    return mCert;
}

bool
PeerAuth::verifyRemoteAuthCert(NodeID const& remoteNode, AuthCert const& cert)
{
    if (cert.expiration < mApp.timeNow())
    {
        CLOG(ERROR, "Overlay")
            << "PeerAuth cert expired: "
            << "expired= " << cert.expiration << ", now=" << mApp.timeNow();
        return false;
    }
    auto hash = sha256(xdr::xdr_to_opaque(
        mApp.getNetworkID(), ENVELOPE_TYPE_AUTH, cert.expiration, cert.pubkey));

    CLOG(DEBUG, "Overlay") << "PeerAuth verifying cert hash: "
                           << hexAbbrev(hash);
    return PubKeyUtils::verifySig(remoteNode, cert.sig, hash);
}

HmacSha256Key
PeerAuth::getSharedKey(Curve25519Public const& remotePublic,
                       Peer::PeerRole role)
{
    auto key = PeerSharedKeyId{remotePublic, role};
    if (mSharedKeyCache.exists(key))
    {
        return mSharedKeyCache.get(key);
    }
    auto value =
        EcdhDeriveSharedKey(mECDHSecretKey, mECDHPublicKey, remotePublic,
                            role == Peer::WE_CALLED_REMOTE);
    mSharedKeyCache.put(key, value);
    return value;
}

HmacSha256Key
PeerAuth::getSendingMacKey(Curve25519Public const& remotePublic,
                           uint256 const& localNonce,
                           uint256 const& remoteNonce, Peer::PeerRole role)
{
    std::vector<uint8_t> buf;
    if (role == Peer::WE_CALLED_REMOTE)
    {
        // If WE_CALLED_REMOTE then sending key is K_AB,
        // and A is local and B is remote.
        buf.push_back(0);
        buf.insert(buf.end(), localNonce.begin(), localNonce.end());
        buf.insert(buf.end(), remoteNonce.begin(), remoteNonce.end());
    }
    else
    {
        // If REMOTE_CALLED_US then sending key is K_BA,
        // and B is local and A is remote.
        buf.push_back(1);
        buf.insert(buf.end(), localNonce.begin(), localNonce.end());
        buf.insert(buf.end(), remoteNonce.begin(), remoteNonce.end());
    }
    auto k = getSharedKey(remotePublic, role);
    return hkdfExpand(k, buf);
}

HmacSha256Key
PeerAuth::getReceivingMacKey(Curve25519Public const& remotePublic,
                             uint256 const& localNonce,
                             uint256 const& remoteNonce, Peer::PeerRole role)
{
    std::vector<uint8_t> buf;
    if (role == Peer::WE_CALLED_REMOTE)
    {
        // If WE_CALLED_REMOTE then receiving key is K_BA,
        // and A is local and B is remote.
        buf.push_back(1);
        buf.insert(buf.end(), remoteNonce.begin(), remoteNonce.end());
        buf.insert(buf.end(), localNonce.begin(), localNonce.end());
    }
    else
    {
        // If REMOTE_CALLED_US then receiving key is K_AB,
        // and B is local and A is remote.
        buf.push_back(0);
        buf.insert(buf.end(), remoteNonce.begin(), remoteNonce.end());
        buf.insert(buf.end(), localNonce.begin(), localNonce.end());
    }
    auto k = getSharedKey(remotePublic, role);
    return hkdfExpand(k, buf);
}
}
