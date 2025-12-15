// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "crypto/SecretKey.h"
#include "xdr/Stellar-ledger-entries.h"

namespace stellar
{

class ByteSlice;
class SecretKey;
struct DecoratedSignature;
struct SignerKey;

namespace SignatureUtils
{

DecoratedSignature sign(SecretKey const& secretKey, Hash const& hash);
PubKeyUtils::VerifySigResult verify(DecoratedSignature const& sig,
                                    SignerKey const& signerKey,
                                    Hash const& hash);
PubKeyUtils::VerifySigResult verify(DecoratedSignature const& sig,
                                    PublicKey const& signerKey,
                                    Hash const& hash);
PubKeyUtils::VerifySigResult
verifyEd25519SignedPayload(DecoratedSignature const& sig,
                           SignerKey const& signer);

DecoratedSignature signHashX(ByteSlice const& x);
bool verifyHashX(DecoratedSignature const& sig, SignerKey const& signerKey);

SignatureHint
getSignedPayloadHint(SignerKey::_ed25519SignedPayload_t const& signedPayload);
SignatureHint getHint(ByteSlice const& bs);
bool doesHintMatch(ByteSlice const& bs, SignatureHint const& hint);
}
}
