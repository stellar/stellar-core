// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/Random.h"
#include <sodium.h>

std::vector<uint8_t>
randomBytes(size_t length)
{
    std::vector<uint8_t> vec(length);
    randombytes_buf(vec.data(), length);
    return vec;
}
