// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/Hex.h"
#include <sodium.h>

namespace stellar
{

std::string
binToHex(ByteSlice const& bin)
{
    // NB: C++ standard says we can't go modifying the contents of a std::string
    // just by const_cast'ing away const on .data(), so we use a vector<char> to
    // write to.
    if (bin.empty())
        return "";
    std::vector<char> hex(bin.size() * 2 + 1, '\0');
    if (sodium_bin2hex(hex.data(), hex.size(), bin.data(), bin.size()) !=
        hex.data())
    {
        throw std::runtime_error(
            "error in stellar::binToHex(std::vector<uint8_t>)");
    }
    return std::string(hex.begin(), hex.end() - 1);
}

std::string
hexAbbrev(ByteSlice const& bin)
{
    size_t sz = bin.size();
    if (sz > 3)
    {
        sz = 3;
    }
    return binToHex(ByteSlice(bin.data(), sz));
}

std::vector<uint8_t>
hexToBin(std::string const& hex)
{
    std::vector<uint8_t> bin(hex.size() / 2, 0);
    if (sodium_hex2bin(bin.data(), bin.size(), hex.data(), hex.size(), NULL,
                       NULL, NULL) != 0)
    {
        throw std::runtime_error("error in stellar::hexToBin(std::string)");
    }
    return bin;
}

uint256
hexToBin256(std::string const& hex)
{
    uint256 out;
    auto bin = hexToBin(hex);
    if (bin.size() != out.size())
    {
        throw std::runtime_error(
            "wrong number of hex bytes when decoding uint256");
    }
    memcpy(out.data(), bin.data(), bin.size());
    return out;
}
}
