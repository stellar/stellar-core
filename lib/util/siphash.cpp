#include "siphash.h"
#include <string.h>

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
    while (m_idx < 7)
    {
        m |= 0 << (m_idx++ * 8);
    }

    m |= ((uint64_t)input_len) << (m_idx * 8);

    digest_block();

    v2 ^= 0xff;

    compress();
    compress();
    compress();
    compress();

    return ((uint64_t)v0 ^ v1 ^ v2 ^ v3);
}
