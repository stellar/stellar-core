// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "crypto/Random.h"
#include <sodium.h>

std::vector<uint8_t>
randomBytes(size_t length)
{
    std::vector<uint8_t> vec(length);
    randombytes_buf(vec.data(), length);
    return vec;
}
