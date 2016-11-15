// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/SignatureUtils.h"

#include "crypto/SecretKey.h"
#include "crypto/SHA.h"
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
    result.hint = PubKeyUtils::getHint(secretKey.getPublicKey());
    return result;
}

bool
verify(DecoratedSignature const& sig, Signer const& signer, Hash const& hash)
{
    auto pubKey = KeyUtils::convertKey<PublicKey>(signer.key);
    return PubKeyUtils::hasHint(pubKey, sig.hint)
        && PubKeyUtils::verifySig(pubKey, sig.signature, hash);
}

DecoratedSignature
signHashX(const ByteSlice &x)
{
    DecoratedSignature result;
    Signature out(x.size(), 0);
    std::memcpy(out.data(), x.data(), x.size());
    result.signature = out;
    return result;
}

bool
verifyHashX(DecoratedSignature const& sig, Signer const& signer)
{
    auto x = std::string{sig.signature.begin(), sig.signature.end()};
    auto hash = sha256(x);
    return signer.key.hashX() == hash;
}

}

}
