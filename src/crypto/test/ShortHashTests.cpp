// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/Random.h"
#include "crypto/ShortHash.h"
#include "ledger/test/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include <autocheck/generator.hpp>

// Confirms that the incremental, non-allocating `xdrComputeHash(...)` produces
// the same output as `computeHash(xdr_to_opaque(...))`.

using namespace stellar;

TEST_CASE("XDR shortHash is identical to byte shortHash", "[shorthash][crypto]")
{
    shortHash::initialize();
    for (size_t i = 0; i < 1000; ++i)
    {
        auto entry = LedgerTestUtils::generateValidLedgerEntry(100);
        auto bytes_hash = shortHash::computeHash(xdr::xdr_to_opaque(entry));
        auto stream_hash = shortHash::xdrComputeHash(entry);
        CHECK(bytes_hash == stream_hash);
    }
}

TEST_CASE("shorthash bytes bench", "[!hide][sh-bytes-bench]")
{
    shortHash::initialize();
    autocheck::rng().seed(11111);
    std::vector<LedgerEntry> entries;
    for (size_t i = 0; i < 1000; ++i)
    {
        entries.emplace_back(LedgerTestUtils::generateValidLedgerEntry(1000));
    }
    for (size_t i = 0; i < 10000; ++i)
    {
        for (auto const& e : entries)
        {
            auto opaque = xdr::xdr_to_opaque(e);
            shortHash::computeHash(opaque);
        }
    }
}

TEST_CASE("shorthash XDR bench", "[!hide][sh-xdr-bench]")
{
    shortHash::initialize();
    autocheck::rng().seed(11111);
    std::vector<LedgerEntry> entries;
    for (size_t i = 0; i < 1000; ++i)
    {
        entries.emplace_back(LedgerTestUtils::generateValidLedgerEntry(1000));
    }
    for (size_t i = 0; i < 10000; ++i)
    {
        for (auto const& e : entries)
        {
            shortHash::xdrComputeHash(e);
        }
    }
}
