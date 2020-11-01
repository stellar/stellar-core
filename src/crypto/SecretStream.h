// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/ByteSlice.h"
#include "sodium/crypto_secretstream_xchacha20poly1305.h"
#include "xdr/Stellar-types.h"
#include <memory>

// This file provides a pair of classes that implement an AEAD stream (both
// encrypted and authenticated by a given shared key).
//
// The stream uses the XChaCha20-Poly1305 AEAD construction, with an internal
// state that stores both the key and a (public) nonce that is randomly
// initialized, and must be conveyed from sender to receiver as a (public)
// "header value" in the initial stream setup. The nonce is then automatically
// incremented on each subsequent message, to avoid reuse.
namespace stellar
{

class SecretStreamProducer
{
    crypto_secretstream_xchacha20poly1305_state mState;
    bool mInitialized{false};

  public:
    void init(OverlayStreamKey const& key, AEADInitHeader& init);
    void encryptNext(ByteSlice const& in, std::vector<uint8_t>& out);
};

class SecretStreamConsumer
{
    crypto_secretstream_xchacha20poly1305_state mState;
    bool mInitialized{false};

  public:
    void init(OverlayStreamKey const& key, AEADInitHeader const& init);
    void decryptNext(ByteSlice const& in, std::vector<uint8_t>& out);
};
}
