// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <autocheck/autocheck.hpp>
#include <lib/catch.hpp>
#include <lib/util/basen.h>

TEST_CASE("base64 tests", "[crypto]")
{
    autocheck::generator<std::vector<uint8_t>> input;
    // check round trip
    for (int s = 0; s < 100; s++)
    {
        std::vector<uint8_t> in(input(s));
        std::string encoded = bn::encode_b64(in);
        std::vector<uint8_t> decoded;

        bn::decode_b64(encoded, decoded);
        REQUIRE(in == decoded);
    }
}
