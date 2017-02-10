#pragma once

// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "xdr/Stellar-types.h"

namespace stellar
{

class ByteSlice;
class SecretKey;
struct DecoratedSignature;
struct SignerKey;

namespace SignatureUtils
{

DecoratedSignature sign(SecretKey const& secretKey, Hash const& hash);
bool verify(DecoratedSignature const& sig, SignerKey const& signerKey,
            Hash const& hash);

DecoratedSignature signHashX(const ByteSlice& x);
bool verifyHashX(DecoratedSignature const& sig, SignerKey const& signerKey);

SignatureHint getHint(ByteSlice const& bs);
bool doesHintMatch(ByteSlice const& bs, SignatureHint const& hint);
}
}
