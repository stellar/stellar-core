// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/SignatureUtils.h"

#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "crypto/SignerKey.h"
#include "xdr/Stellar-transaction.h"

namespace stellar
{

namespace SignatureUtils
{

DecoratedSignature
sign(SecretKey const& secretKey, Hash const& hash)
{
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
    if (!doesHintMatch(pubKey.ed25519(), sig.hint))
        return false;

    return PubKeyUtils::verifySig(pubKey, sig.signature, hash);
}

DecoratedSignature
signHashX(const ByteSlice& x)
{
    DecoratedSignature result;
    Signature out(0, 0);
    out.resize(static_cast<uint32_t>(x.size()));
    std::memcpy(out.data(), x.data(), x.size());
    result.signature = out;
    result.hint = getHint(sha256(x));
    return result;
}

bool
verifyHashX(DecoratedSignature const& sig, SignerKey const& signerKey)
{
    if (!doesHintMatch(signerKey.hashX(), sig.hint))
        return false;

    return signerKey.hashX() == sha256(sig.signature);
}

SignatureHint
getHint(ByteSlice const& bs)
{
    SignatureHint res;
    memcpy(res.data(), bs.end() - res.size(), res.size());
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
