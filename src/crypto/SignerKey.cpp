// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SignerKey.h"

#include "crypto/StrKey.h"
#include "xdr/Stellar-types.h"

#include <sodium/crypto_sign.h>

namespace stellar
{

std::string
KeyFunctions<SignerKey>::getKeyTypeName()
{
    return "signer key";
}

bool
KeyFunctions<SignerKey>::getKeyVersionIsSupported(
    strKey::StrKeyVersionByte keyVersion)
{
    switch (keyVersion)
    {
    case strKey::STRKEY_PUBKEY_ED25519:
        return true;
    case strKey::STRKEY_PRE_AUTH_TX:
        return true;
    case strKey::STRKEY_HASH_X:
        return true;
    default:
        return false;
    }
}

typename KeyFunctions<SignerKey>::getKeyTypeEnum::type
KeyFunctions<SignerKey>::toKeyType(strKey::StrKeyVersionByte keyVersion)
{
    switch (keyVersion)
    {
    case strKey::STRKEY_PUBKEY_ED25519:
        return SignerKeyType::SIGNER_KEY_TYPE_ED25519;
    case strKey::STRKEY_PRE_AUTH_TX:
        return SignerKeyType::SIGNER_KEY_TYPE_PRE_AUTH_TX;
    case strKey::STRKEY_HASH_X:
        return SignerKeyType::SIGNER_KEY_TYPE_HASH_X;
    default:
        throw std::invalid_argument("invalid signer key type");
    }
}

strKey::StrKeyVersionByte
KeyFunctions<SignerKey>::toKeyVersion(SignerKeyType keyType)
{
    switch (keyType)
    {
    case SignerKeyType::SIGNER_KEY_TYPE_ED25519:
        return strKey::STRKEY_PUBKEY_ED25519;
    case SignerKeyType::SIGNER_KEY_TYPE_PRE_AUTH_TX:
        return strKey::STRKEY_PRE_AUTH_TX;
    case SignerKeyType::SIGNER_KEY_TYPE_HASH_X:
        return strKey::STRKEY_HASH_X;
    default:
        throw std::invalid_argument("invalid signer key type");
    }
}

uint256&
KeyFunctions<SignerKey>::getKeyValue(SignerKey& key)
{
    switch (key.type())
    {
    case SIGNER_KEY_TYPE_ED25519:
        return key.ed25519();
    case SIGNER_KEY_TYPE_PRE_AUTH_TX:
        return key.preAuthTx();
    case SIGNER_KEY_TYPE_HASH_X:
        return key.hashX();
    default:
        throw std::invalid_argument("invalid signer key type");
    }
}

uint256 const&
KeyFunctions<SignerKey>::getKeyValue(SignerKey const& key)
{
    switch (key.type())
    {
    case SIGNER_KEY_TYPE_ED25519:
        return key.ed25519();
    case SIGNER_KEY_TYPE_PRE_AUTH_TX:
        return key.preAuthTx();
    case SIGNER_KEY_TYPE_HASH_X:
        return key.hashX();
    default:
        throw std::invalid_argument("invalid signer key type");
    }
}
}
