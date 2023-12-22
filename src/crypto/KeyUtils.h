#pragma once

// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/StrKey.h"
#include "util/SecretValue.h"
#include "xdr/Stellar-types.h"

#include <sodium.h>

#include <string>

namespace stellar
{

class SecretKey;

template <typename T> struct KeyFunctions
{
    struct getKeyTypeEnum
    {
    };

    static std::string getKeyTypeName();
    static bool getKeyVersionIsSupported(strKey::StrKeyVersionByte keyVersion);
    static bool
    getKeyVersionIsVariableLength(strKey::StrKeyVersionByte keyVersion);
    static typename getKeyTypeEnum::type
    toKeyType(strKey::StrKeyVersionByte keyVersion);
    static strKey::StrKeyVersionByte
    toKeyVersion(typename getKeyTypeEnum::type keyType);
    static uint256& getEd25519Value(T& key);
    static uint256 const& getEd25519Value(T const& key);

    static std::vector<uint8_t> getKeyValue(T const& key);
    static void setKeyValue(T& key, std::vector<uint8_t> const& data);
};

// signer key utility functions
namespace KeyUtils
{

template <typename T>
typename std::enable_if<!std::is_same<T, SecretKey>::value, std::string>::type
toStrKey(T const& key)
{
    return strKey::toStrKey(KeyFunctions<T>::toKeyVersion(key.type()),
                            KeyFunctions<T>::getKeyValue(key))
        .value;
}

template <typename T>
typename std::enable_if<std::is_same<T, SecretKey>::value, SecretValue>::type
toStrKey(T const& key)
{
    return strKey::toStrKey(KeyFunctions<T>::toKeyVersion(key.type()),
                            KeyFunctions<T>::getKeyValue(key));
}

template <typename T>
typename std::enable_if<!std::is_same<T, SecretKey>::value, std::string>::type
toShortString(T const& key)
{
    return toStrKey(key).substr(0, 5);
}

template <typename T>
typename std::enable_if<std::is_same<T, SecretKey>::value, SecretValue>::type
toShortString(T const& key)
{
    return SecretValue{toStrKey(key).value.substr(0, 5)};
}

std::size_t getKeyVersionSize(strKey::StrKeyVersionByte keyVersion);

// An exception representing an invalid string key representation
struct InvalidStrKey : public std::invalid_argument
{
    using std::invalid_argument::invalid_argument;
};

template <typename T>
T
fromStrKey(std::string const& s)
{
    T key;
    uint8_t verByte;
    std::vector<uint8_t> k;
    if (!strKey::fromStrKey(s, verByte, k))
    {
        throw InvalidStrKey("bad " + KeyFunctions<T>::getKeyTypeName());
    }

    strKey::StrKeyVersionByte ver =
        static_cast<strKey::StrKeyVersionByte>(verByte);

    bool fixedSizeKeyValid =
        !KeyFunctions<T>::getKeyVersionIsVariableLength(ver) &&
        (k.size() != getKeyVersionSize(ver));
    if (fixedSizeKeyValid || !KeyFunctions<T>::getKeyVersionIsSupported(ver) ||
        s.size() != strKey::getStrKeySize(k.size()))
    {
        throw InvalidStrKey("bad " + KeyFunctions<T>::getKeyTypeName());
    }

    key.type(KeyFunctions<T>::toKeyType(ver));
    KeyFunctions<T>::setKeyValue(key, k);
    return key;
}

template <typename T, typename F>
bool
canConvert(F const& fromKey)
{
    return KeyFunctions<T>::getKeyVersionIsSupported(
        KeyFunctions<F>::toKeyVersion(fromKey.type()));
}

template <typename T, typename F>
T
convertKey(F const& fromKey)
{
    T toKey;
    toKey.type(KeyFunctions<T>::toKeyType(
        KeyFunctions<F>::toKeyVersion(fromKey.type())));
    KeyFunctions<T>::getEd25519Value(toKey) =
        KeyFunctions<F>::getEd25519Value(fromKey);
    return toKey;
}
}
}
