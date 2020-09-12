#include "siphash.h"
#include <string.h>
#include <cassert>

SipHash24::SipHash24(const unsigned char key[16])
{
    uint64_t k0 = load_le_64(key);
    uint64_t k1 = load_le_64(key + 8);

    this->v0 = (0x736f6d6570736575 ^ k0);
    this->v1 = (0x646f72616e646f6d ^ k1);
    this->v2 = (0x6c7967656e657261 ^ k0);
    this->v3 = (0x7465646279746573 ^ k1);

    this->m_idx = 0;
    this->input_len = 0;
    this->m = 0;
}

SipHash24::~SipHash24()
{
}

uint64_t
SipHash24::digest()
{
    // According to the SipHash paper, the final 64-bit LE word of input should
    // contain "the last 0 through 7 bytes of m followed by null bytes and
    // ending with a byte encoding the positive integer b mod 256".
    //
    // Since we reset m to 0 each time we start filling it, only its low bytes
    // (those with values copied from the input) are nonzero at this point; the
    // only byte of 'm' we have to write to is therefore the MSB containing the
    // length-mod-256. But for safety sake we assert that the zero bytes we're
    // expecting are in fact all zero.

    uint64_t mask = 0xff;
    mask <<= m_idx * 8;
    while (m_idx < 7)
    {
        assert((m & mask) == 0);
        mask <<= 8;
        m_idx++;
    }

    assert((m & mask) == 0);
    assert(m_idx == 7);
    m |= ((uint64_t)input_len) << (m_idx * 8);

    digest_block();

    v2 ^= 0xff;

    compress();
    compress();
    compress();
    compress();

    return ((uint64_t)v0 ^ v1 ^ v2 ^ v3);
}
