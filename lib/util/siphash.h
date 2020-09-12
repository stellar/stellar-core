#pragma once

// Adapted from https://github.com/whitfin/siphash-cpp
// Copyright 2016 Isaac Whitfield
// Licensed under the MIT license
// http://opensource.org/licenses/MIT

#include <cstddef>
#include <cstdint>

class SipHash24
{
  private:
    // The next byte within m, from LSB to MSB, that update() will write to.
    int m_idx;
    uint64_t v0, v1, v2, v3, m;
    unsigned char input_len;

    static uint64_t
    rotate_left(uint64_t x, int b)
    {
        return ((x << b) | (x >> (64 - b)));
    }

    static uint64_t
    load_le_64(uint8_t const* p)
    {
        // NB: LLVM will boil this down to a single 64bit load on an
        // LE target.
        return (((uint64_t)((p)[0])) | ((uint64_t)((p)[1]) << 8) |
                ((uint64_t)((p)[2]) << 16) | ((uint64_t)((p)[3]) << 24) |
                ((uint64_t)((p)[4]) << 32) | ((uint64_t)((p)[5]) << 40) |
                ((uint64_t)((p)[6]) << 48) | ((uint64_t)((p)[7]) << 56));
    }

    void
    compress()
    {
        v0 += v1;
        v1 = rotate_left(v1, 13);
        v1 ^= v0;
        v0 = rotate_left(v0, 32);
        v2 += v3;
        v3 = rotate_left(v3, 16);
        v3 ^= v2;
        v0 += v3;
        v3 = rotate_left(v3, 21);
        v3 ^= v0;
        v2 += v1;
        v1 = rotate_left(v1, 17);
        v1 ^= v2;
        v2 = rotate_left(v2, 32);
    }

    void
    digest_block()
    {
        v3 ^= m;
        compress();
        compress();
        v0 ^= m;
        m_idx = 0;
        m = 0;
    }

  public:
    SipHash24(uint8_t const key[16]);
    ~SipHash24();

    void
    update(uint8_t const* data, size_t len)
    {
        // If we're starting at the LSB of m (m_idx == 0)
        // then we can do an early word-at-a-time loop
        // until we get to the remainder.
        uint8_t const* end = data + len;
        if (m_idx == 0)
        {
            uint8_t const* end8 = data + (len & ~7);
            for (; data != end8; data += 8)
            {
                input_len += 8;
                m = load_le_64(data);
                digest_block();
            }
        }
        // Then at the remainder we go byte-at-a-time.
        for (; data != end; ++data)
        {
            input_len++;
            m |= (static_cast<uint64_t>(*data) << (m_idx++ * 8));
            if (m_idx >= 8)
            {
                digest_block();
            }
        }
    }
    uint64_t digest();
};
