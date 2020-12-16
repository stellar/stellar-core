// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/Bucket.h"
#include "ledger/test/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "test/test.h"
#include "util/Logging.h"
#include "util/XDRStream.h"
#include <fmt/format.h>

#include <chrono>

using namespace stellar;

TEST_CASE("XDROutputFileStream fail modes", "[xdrstream]")
{
    VirtualClock clock;
    XDROutputFileStream out(clock.getIOContext(), /*doFsync=*/true);
    auto filename = "someFile";

    SECTION("open throws")
    {
        REQUIRE_NOTHROW(out.open(filename));
        // File is already open
        REQUIRE_THROWS_AS(out.open(filename), std::runtime_error);
        std::remove(filename);
    }
    SECTION("write throws")
    {
        SHA256 hasher;
        size_t bytes = 0;
        auto ledgerEntries = LedgerTestUtils::generateValidLedgerEntries(1);
        auto bucketEntries =
            Bucket::convertToBucketEntry(false, {}, ledgerEntries, {});

        REQUIRE_THROWS_AS(out.writeOne(bucketEntries[0], &hasher, &bytes),
                          std::runtime_error);
    }
    SECTION("close throws")
    {
        REQUIRE_THROWS_AS(out.close(), std::runtime_error);
    }
}

TEST_CASE("XDROutputFileStream fsync bench", "[!hide][xdrstream][bench]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig(0);

    SHA256 hasher;
    auto ledgerEntries = LedgerTestUtils::generateValidLedgerEntries(10000000);
    auto bucketEntries =
        Bucket::convertToBucketEntry(false, {}, ledgerEntries, {});

    fs::mkpath(cfg.BUCKET_DIR_PATH);

    for (int i = 0; i < 10; ++i)
    {
        XDROutputFileStream outFsync(clock.getIOContext(), /*doFsync=*/true);
        XDROutputFileStream outNoFsync(clock.getIOContext(), /*doFsync=*/false);

        outFsync.open(
            fmt::format("{}/outFsync-{}.xdr", cfg.BUCKET_DIR_PATH, i));
        outNoFsync.open(
            fmt::format("{}/outNoFsync-{}.xdr", cfg.BUCKET_DIR_PATH, i));

        size_t bytes = 0;
        auto start = std::chrono::system_clock::now();
        for (auto const& e : bucketEntries)
        {
            outFsync.writeOne(e, &hasher, &bytes);
        }
        outFsync.close();
        auto stop = std::chrono::system_clock::now();
        auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);
        CLOG_INFO(Fs, "wrote {} bytes to fsync file in {}ms", bytes,
                  elapsed.count());

        bytes = 0;
        start = std::chrono::system_clock::now();
        for (auto const& e : bucketEntries)
        {
            outNoFsync.writeOne(e, &hasher, &bytes);
        }
        outNoFsync.close();
        stop = std::chrono::system_clock::now();
        elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);
        CLOG_INFO(Fs, "wrote {} bytes to no-fsync file in {}ms", bytes,
                  elapsed.count());
    }
}
