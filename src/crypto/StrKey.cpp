// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "StrKey.h"
#include "util/SecretValue.h"
#include "util/basen.h"
#include "util/crc16.h"

namespace stellar
{
namespace strKey
{
// Encode a version byte and ByteSlice into StrKey
SecretValue
toStrKey(uint8_t ver, ByteSlice const& bin)
{
    ver <<= 3; // promote to 8 bits
    std::vector<uint8_t> toEncode;
    toEncode.reserve(1 + bin.size() + 2);
    toEncode.emplace_back(ver);
    toEncode.insert(toEncode.end(), bin.begin(), bin.end());

    uint16_t crc = crc16((char*)toEncode.data(), (int)toEncode.size());
    toEncode.emplace_back(static_cast<uint8_t>(crc & 0xFF));
    crc >>= 8;
    toEncode.emplace_back(static_cast<uint8_t>(crc & 0xFF));

    std::string res;
    res = bn::encode_b32(toEncode);
    return SecretValue{res};
}

size_t
getStrKeySize(size_t dataSize)
{
    dataSize += 3; // version and crc
    return bn::encoded_size32(dataSize);
}

bool
fromStrKey(std::string const& strKey, uint8_t& outVersion,
           std::vector<uint8_t>& decoded)
{
    bn::decode_b32(strKey, decoded);
    if (decoded.size() < 3)
    {
        return false;
    }
    uint16_t crc = 0;
    crc = decoded.back();
    decoded.pop_back();
    crc <<= 8;
    crc |= decoded.back();
    decoded.pop_back();

    if (crc16((char*)decoded.data(), (int)decoded.size()) != crc)
    {
        return false;
    }

    outVersion = decoded.at(0) >> 3; // only keep 5 bits from the version
    decoded.erase(decoded.begin());

    return true;
}
}
}