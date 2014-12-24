// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "main/test.h"
#include "util/Logging.h"
#include "lib/catch.hpp"
#include <sodium.h>

TEST_CASE("libsodium smoketest", "[crypto]")
{
    std::string in = "hello world";
    std::string expect = "309ecc489c12d6eb4cc40f50c902f2b4d0ed77ee511a7c7a9bcd3ca86d4cd86f989dd35bc5ff499670da34255b45b0cfd830e81f605dcf7dc5542e93ae9cd76f";
    unsigned char out[crypto_hash_sha512_BYTES];
    char hex[crypto_hash_sha512_BYTES * 2 + 1];

    CHECK(sizeof(hex) == expect.size() + 1);
    CHECK(crypto_hash_sha512(out, (const unsigned char*)in.c_str(), in.size()) == 0);
    CHECK(sodium_bin2hex(hex, sizeof(hex), out, sizeof(out)) == hex);
    CHECK(memcmp(expect.c_str(), hex, expect.size()) == 0);
    LOG(DEBUG) << "crypto_hash_sha512(\"hello world\") == \"" << std::string(hex).substr(0, 10) << "...\"";
}
