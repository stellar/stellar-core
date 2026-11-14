// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/LtHash.h"
#include "crypto/SHA.h"
#include "util/GlobalChecks.h"
#include <cstring>
#include <sodium.h>

namespace stellar
{

namespace
{
char const LTHASH_DOMAIN[] = "stellar lthash16 v1";
}

LtHash16::LtHash16()
{
    mElements.fill(0);
}

void
LtHash16::expand(ByteSlice const& member, std::array<uint16_t, ELEMENTS>& out)
{
    static_assert(crypto_stream_chacha20_ietf_KEYBYTES == 32,
                  "unexpected ChaCha20 key size");
    SHA256 keyHasher;
    keyHasher.add(ByteSlice(LTHASH_DOMAIN, sizeof(LTHASH_DOMAIN) - 1));
    keyHasher.add(member);
    uint256 key = keyHasher.finish();

    unsigned char nonce[crypto_stream_chacha20_ietf_NONCEBYTES] = {0};
    std::array<uint8_t, STATE_BYTES> stream;
    int res = crypto_stream_chacha20_ietf(stream.data(), stream.size(), nonce,
                                          key.data());
    releaseAssert(res == 0);
    for (size_t i = 0; i < ELEMENTS; ++i)
    {
        out[i] = static_cast<uint16_t>(stream[2 * i]) |
                 static_cast<uint16_t>(static_cast<uint16_t>(stream[2 * i + 1])
                                       << 8);
    }
}

void
LtHash16::add(ByteSlice const& member)
{
    std::array<uint16_t, ELEMENTS> e;
    expand(member, e);
    for (size_t i = 0; i < ELEMENTS; ++i)
    {
        mElements[i] = static_cast<uint16_t>(mElements[i] + e[i]);
    }
}

void
LtHash16::remove(ByteSlice const& member)
{
    std::array<uint16_t, ELEMENTS> e;
    expand(member, e);
    for (size_t i = 0; i < ELEMENTS; ++i)
    {
        mElements[i] = static_cast<uint16_t>(mElements[i] - e[i]);
    }
}

void
LtHash16::add(LtHash16 const& other)
{
    for (size_t i = 0; i < ELEMENTS; ++i)
    {
        mElements[i] = static_cast<uint16_t>(mElements[i] + other.mElements[i]);
    }
}

void
LtHash16::remove(LtHash16 const& other)
{
    for (size_t i = 0; i < ELEMENTS; ++i)
    {
        mElements[i] = static_cast<uint16_t>(mElements[i] - other.mElements[i]);
    }
}

bool
LtHash16::isZero() const
{
    for (auto e : mElements)
    {
        if (e != 0)
        {
            return false;
        }
    }
    return true;
}

bool
LtHash16::operator==(LtHash16 const& other) const
{
    return mElements == other.mElements;
}

std::array<uint8_t, LtHash16::STATE_BYTES>
LtHash16::toBytes() const
{
    std::array<uint8_t, STATE_BYTES> bytes;
    for (size_t i = 0; i < ELEMENTS; ++i)
    {
        bytes[2 * i] = static_cast<uint8_t>(mElements[i] & 0xff);
        bytes[2 * i + 1] = static_cast<uint8_t>(mElements[i] >> 8);
    }
    return bytes;
}

LtHash16
LtHash16::fromBytes(std::array<uint8_t, STATE_BYTES> const& bytes)
{
    LtHash16 res;
    for (size_t i = 0; i < ELEMENTS; ++i)
    {
        res.mElements[i] =
            static_cast<uint16_t>(bytes[2 * i]) |
            static_cast<uint16_t>(static_cast<uint16_t>(bytes[2 * i + 1]) << 8);
    }
    return res;
}

uint256
LtHash16::digest() const
{
    auto bytes = toBytes();
    return sha256(ByteSlice(bytes.data(), bytes.size()));
}
}
