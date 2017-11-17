// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/SignatureUtils.h"
#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "crypto/SignerKey.h"
#include "crypto/SignerKeyUtils.h"
#include "lib/catch.hpp"
#include "xdr/Stellar-transaction.h"

using namespace stellar;

TEST_CASE("Pubkey signature", "[signature]")
{
    for (auto i = 0; i < 10; i++)
    {
        auto secretKey = SecretKey::fromSeed(
            sha256(std::string{"NODE_SEED_"} + std::to_string(i)));
        for (auto j = 0; j < 10; j++)
        {
            auto hash = sha256(std::string{"HASH_"} + std::to_string(i) +
                               std::to_string(j));
            auto signature = SignatureUtils::sign(secretKey, hash);
            REQUIRE(SignatureUtils::verify(
                signature,
                KeyUtils::convertKey<SignerKey>(secretKey.getPublicKey()),
                hash));
        }
    }
}

TEST_CASE("Hash(x) signature", "[signature]")
{
    for (size_t i = 0; i < 65; i++)
    {
        auto s = std::string(i, 'A');
        auto signerKey = SignerKeyUtils::hashXKey(s);
        auto signature = SignatureUtils::signHashX(s);
        REQUIRE(SignatureUtils::verifyHashX(signature, signerKey));
    }
    for (size_t i = 65; i < 100; i++)
    {
        auto s = std::string(i, 'A');
        REQUIRE_THROWS_AS(SignatureUtils::signHashX(s), xdr::xdr_overflow);
    }
}
