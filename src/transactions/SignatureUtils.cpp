// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/SignatureUtils.h"

#include "crypto/SecretKey.h"
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

DecoratedSignature
signHashX(const ByteSlice &x)
{
    DecoratedSignature result;
    Signature out(x.size(), 0);
    std::memcpy(out.data(), x.data(), x.size());
    result.signature = out;
    return result;
}

}

}
