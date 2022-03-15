// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/SignatureUtils.h"

#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "crypto/SignerKey.h"
#include "util/GlobalChecks.h"
#include "xdr/Stellar-transaction.h"
#include <Tracy.hpp>

namespace stellar
{

namespace SignatureUtils
{

DecoratedSignature
sign(SecretKey const& secretKey, Hash const& hash)
{
    ZoneScoped;
    DecoratedSignature result;
    result.signature = secretKey.sign(hash);
    result.hint = getHint(secretKey.getPublicKey().ed25519());
    return result;
}

bool
verify(DecoratedSignature const& sig, SignerKey const& signerKey,
       Hash const& hash)
{
    auto pubKey = KeyUtils::convertKey<PublicKey>(signerKey);
    return verify(sig, pubKey, hash);
}

bool
verify(DecoratedSignature const& sig, PublicKey const& pubKey, Hash const& hash)
{
    if (!doesHintMatch(pubKey.ed25519(), sig.hint))
    {
        return false;
    }
    return PubKeyUtils::verifySig(pubKey, sig.signature, hash);
}

bool
verifyEd25519SignedPayload(DecoratedSignature const& sig,
                           SignerKey const& signer)
{
    auto const& signedPayload = signer.ed25519SignedPayload();

    if (!doesHintMatch(getSignedPayloadHint(signedPayload), sig.hint))
        return false;

    PublicKey pubKey;
    pubKey.ed25519() = signedPayload.ed25519;

    return PubKeyUtils::verifySig(pubKey, sig.signature, signedPayload.payload);
}

DecoratedSignature
signHashX(const ByteSlice& x)
{
    ZoneScoped;
    DecoratedSignature result;
    Signature out(0, 0);
    out.resize(static_cast<uint32_t>(x.size()));
    if (!x.empty() && x.data())
    {
        std::memcpy(out.data(), x.data(), x.size());
    }
    else
    {
        releaseAssertOrThrow(x.empty());
    }
    result.signature = out;
    result.hint = getHint(sha256(x));
    return result;
}

bool
verifyHashX(DecoratedSignature const& sig, SignerKey const& signerKey)
{
    ZoneScoped;
    if (!doesHintMatch(signerKey.hashX(), sig.hint))
        return false;

    return signerKey.hashX() == sha256(sig.signature);
}

SignatureHint
getSignedPayloadHint(SignerKey::_ed25519SignedPayload_t const& signedPayload)
{
    auto pubKeyHint = getHint(signedPayload.ed25519);
    auto payloadHint = getHint(signedPayload.payload);
    SignatureHint hint;
    hint[0] = pubKeyHint[0] ^ payloadHint[0];
    hint[1] = pubKeyHint[1] ^ payloadHint[1];
    hint[2] = pubKeyHint[2] ^ payloadHint[2];
    hint[3] = pubKeyHint[3] ^ payloadHint[3];

    return hint;
}

SignatureHint
getHint(ByteSlice const& bs)
{
    if (bs.empty())
    {
        return SignatureHint();
    }

    SignatureHint res;
    if (res.size() > bs.size())
    {
        memcpy(res.data(), bs.begin(), bs.size());
    }
    else
    {
        memcpy(res.data(), bs.end() - res.size(), res.size());
    }
    return res;
}

bool
doesHintMatch(ByteSlice const& bs, SignatureHint const& hint)
{
    if (bs.size() < hint.size())
    {
        return false;
    }

    return memcmp(bs.end() - hint.size(), hint.data(), hint.size()) == 0;
}
}
}
