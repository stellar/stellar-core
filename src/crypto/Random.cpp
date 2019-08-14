// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/Random.h"
#include "util/Math.h"

#include <algorithm>
#include <sodium.h>

#ifdef MSAN_ENABLED
#include <sanitizer/msan_interface.h>
#endif

std::vector<uint8_t>
randomBytes(size_t length)
{
    std::vector<uint8_t> vec(length);

#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    std::uniform_int_distribution<unsigned short> dist(0, 255);
    std::generate(vec.begin(), vec.end(), [&]() {
        return static_cast<uint8_t>(dist(stellar::gRandomEngine));
    });
#else
    randombytes_buf(vec.data(), length);
#endif

#ifdef MSAN_ENABLED
    __msan_unpoison(out.key.data(), out.key.size());
#endif
    return vec;
}
