// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "KeyUtils.h"

#include "crypto/StrKey.h"

namespace stellar
{

size_t
KeyUtils::getKeyVersionSize(strKey::StrKeyVersionByte keyVersion)
{
    switch (keyVersion)
    {
    case strKey::STRKEY_PUBKEY_ED25519:
    case strKey::STRKEY_SEED_ED25519:
        return crypto_sign_PUBLICKEYBYTES;
    case strKey::STRKEY_PRE_AUTH_TX:
    case strKey::STRKEY_HASH_X:
        return 32U;
    default:
        throw std::invalid_argument("invalid key version: " +
                                    std::to_string(keyVersion));
    }
}
}
