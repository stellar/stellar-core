// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "crypto/Base58.h"
#include "crypto/SHA.h"
#include <algorithm>

namespace stellar
{

std::string const bitcoinBase58Alphabet =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

std::string const stellarBase58Alphabet =
    "gsphnaf39wBUDNEGHJKLM4PQRST7VWXYZ2bcdeCr65jkm8oFqi1tuvAxyz";

// Non-bignum base58 encoding routines adapted from bs58.js which in turn
// was adapted from
// https://github.com/bitcoin/bitcoin/blob/master/src/base58.cpp

std::string
baseEncode(std::string const& alphabet, ByteSlice const& bytes)
{
    if (bytes.size() == 0)
    {
        return "";
    }

    uint16_t base = static_cast<uint16_t>(alphabet.size());
    std::vector<uint16_t> digits{0};

    // Propagate each byte + carry.
    for (auto byte : bytes)
    {
        for (auto& digit : digits)
        {
            digit <<= 8;
        }

        digits[0] += byte;

        uint16_t carry = 0;
        for (auto& digit : digits)
        {
            digit += carry;
            carry = digit / base;
            digit %= base;
        }

        while (carry != 0)
        {
            digits.push_back(carry % base);
            carry /= base;
        }
    }

    // Append leading zeroes.
    for (size_t i = 0; bytes[i] == 0 && i < bytes.size() - 1; ++i)
    {
        digits.push_back(0);
    }

    // Alphabet-ize and reverse.
    std::string ret(digits.size(), ' ');
    std::transform(digits.rbegin(), digits.rend(), ret.begin(),
                   [&alphabet](uint8_t digit)
                   {
        return alphabet.at(digit);
    });
    return ret;
}

std::vector<uint8_t>
baseDecode(std::string const& alphabet, std::string const& encoded)
{

    if (encoded.size() == 0)
    {
        return std::vector<uint8_t>();
    }

    std::vector<uint16_t> bytes = {0};

    std::vector<uint16_t> alphabetMap(256, 0xffff);
    for (size_t i = 0; i < alphabet.size(); ++i)
        alphabetMap.at(static_cast<uint8_t>(alphabet.at(i))) =
            static_cast<uint16_t>(i);
    uint16_t base = static_cast<uint16_t>(alphabet.size());

    // Propagate each character + carry.
    for (auto c : encoded)
    {
        for (auto& byte : bytes)
        {
            byte *= base;
        }

        auto n = alphabetMap.at(static_cast<uint8_t>(c));
        if (n == 0xffff)
        {
            throw std::runtime_error(
                "unknown character in stellar::baseDecode");
        }

        bytes[0] += n;

        uint16_t carry = 0;
        for (auto& byte : bytes)
        {
            byte += carry;
            carry = byte >> 8;
            byte &= 0xff;
        }

        while (carry != 0)
        {
            bytes.push_back(carry & 0xff);
            carry >>= 8;
        }
    }

    // Append leading zeroes.
    for (size_t i = 0;
         encoded.at(i) == alphabet.at(0) && i < encoded.size() - 1; i++)
    {
        bytes.push_back(0);
    }

    // Trim to real bytes and reverse.
    std::vector<uint8_t> ret(bytes.size(), 0);
    std::transform(bytes.rbegin(), bytes.rend(), ret.begin(), [](uint16_t i)
                   {
        return static_cast<uint8_t>(i);
    });
    return ret;
}

std::string
baseCheckEncode(std::string const& alphabet, uint8_t ver,
                ByteSlice const& bytes)
{
    std::vector<uint8_t> vb(bytes.size() + 1, 0);
    vb.at(0) = ver;
    std::copy(bytes.begin(), bytes.end(), vb.begin() + 1);
    uint256 hash = sha256(sha256(vb));
    vb.insert(vb.end(), hash.begin(), hash.begin() + 4);
    return baseEncode(alphabet, vb);
}

std::pair<uint8_t, std::vector<uint8_t>>
baseCheckDecode(std::string const& alphabet, std::string const& encoded)
{
    std::vector<uint8_t> bytes = baseDecode(alphabet, encoded);
    if (bytes.size() < 5)
    {
        throw std::runtime_error("baseCheckDecode decoded to <5 bytes");
    }
    uint256 hash = sha256(sha256(ByteSlice(bytes.data(), bytes.size() - 4)));
    if (!std::equal(hash.begin(), hash.begin() + 4,
                    bytes.begin() + (bytes.size() - 4)))
    {
        throw std::runtime_error("baseCheckDecode checksum failed");
    }
    return std::make_pair(
        bytes[0], std::vector<uint8_t>(bytes.begin() + 1,
                                       bytes.begin() + (bytes.size() - 4)));
}

std::string
toBase58Check(Base58CheckVersionByte ver, ByteSlice const& bytes)
{
    return baseCheckEncode(stellarBase58Alphabet, static_cast<uint8_t>(ver),
                           bytes);
}

std::pair<Base58CheckVersionByte, std::vector<uint8_t>>
fromBase58Check(std::string const& encoded)
{
    auto pair = baseCheckDecode(stellarBase58Alphabet, encoded);
    return std::make_pair(static_cast<Base58CheckVersionByte>(pair.first),
                          pair.second);
}

uint256
fromBase58Check256(Base58CheckVersionByte expect, std::string const& encoded)
{
    uint256 out;
    auto p = fromBase58Check(encoded);
    if (p.first != expect)
    {
        throw std::runtime_error("unexpected base58 version byte");
    }
    if (p.second.size() != out.size())
    {
        throw std::runtime_error(
            "unexpected base58 length when decoding uint256");
    }
    std::copy(p.second.begin(), p.second.end(), out.begin());
    return out;
}
}
