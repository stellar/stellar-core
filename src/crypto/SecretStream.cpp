// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SecretStream.h"
#include "crypto/CryptoError.h"
#include "util/GlobalChecks.h"

namespace stellar
{

void
SecretStreamProducer::init(OverlayStreamKey const& key, AEADInitHeader& init)
{
    releaseAssert(!mInitialized);
    // AEAD key (used in overlay v16 and onward) is 32 bytes, the same as the
    // HMAC key size used in overlay v15 and before.
    releaseAssert(key.key.size() ==
                  crypto_secretstream_xchacha20poly1305_KEYBYTES);
    releaseAssert(init.header.size() ==
                  crypto_secretstream_xchacha20poly1305_HEADERBYTES);
    int res = crypto_secretstream_xchacha20poly1305_init_push(
        &mState, init.header.data(), key.key.data());
    if (res != 0)
    {
        throw CryptoError("error from "
                          "crypto_secretstream_xchacha20poly1305_init_push");
    }
    mInitialized = true;
}

void
SecretStreamProducer::encryptNext(ByteSlice const& in,
                                  std::vector<uint8_t>& out)
{
    releaseAssert(mInitialized);
    unsigned char tag = crypto_secretstream_xchacha20poly1305_TAG_MESSAGE;
    out.resize(in.size() + crypto_secretstream_xchacha20poly1305_ABYTES);
    int res = crypto_secretstream_xchacha20poly1305_push(
        &mState,
        /*c=*/out.data(), /*clen_p=*/nullptr,
        /*m=*/in.data(), /*mlen=*/in.size(),
        /*ad=*/nullptr, /*adlen=*/0, tag);
    if (res != 0)
    {
        throw CryptoError("error from "
                          "crypto_secretstream_xchacha20poly1305_push");
    }
}

void
SecretStreamConsumer::init(OverlayStreamKey const& key,
                           AEADInitHeader const& init)
{
    releaseAssert(key.key.size() ==
                  crypto_secretstream_xchacha20poly1305_KEYBYTES);
    releaseAssert(init.header.size() ==
                  crypto_secretstream_xchacha20poly1305_HEADERBYTES);
    int res = crypto_secretstream_xchacha20poly1305_init_pull(
        &mState, init.header.data(), key.key.data());
    if (res != 0)
    {
        throw CryptoError("error from "
                          "crypto_secretstream_xchacha20poly1305_init_pull");
    }
    mInitialized = true;
}

void
SecretStreamConsumer::decryptNext(ByteSlice const& in,
                                  std::vector<uint8_t>& out)
{
    releaseAssert(mInitialized);
    out.resize(in.size() - crypto_secretstream_xchacha20poly1305_ABYTES);
    int res = crypto_secretstream_xchacha20poly1305_pull(
        &mState, /*m=*/out.data(), /*mlen_p=*/nullptr,
        /*tag_p=*/nullptr,
        /*c=*/in.data(), /*clen=*/in.size(),
        /*ad=*/nullptr, /*adlen=*/0);
    if (res != 0)
    {
        throw CryptoError("error from "
                          "crypto_secretstream_xchacha20poly1305_pull");
    }
}
}
