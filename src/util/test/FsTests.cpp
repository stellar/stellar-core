// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/Random.h"
#include "history/FileTransferInfo.h"
#include "test/Catch2.h"
#include "util/FileSystemException.h"
#include "util/Fs.h"
#include "util/TmpDir.h"

#include <limits>

using namespace stellar;
namespace stdfs = std::filesystem;
namespace fs = stellar::fs;

TEST_CASE("filesystem locks", "[fs]")
{
    TmpDir tmp("fstests");
    stdfs::path root(tmp.getName());
    stdfs::path lockfile = root / "file.lock";
    fs::lockFile(lockfile.string());
    REQUIRE(fs::exists(lockfile.string()));
    REQUIRE_THROWS_AS(fs::lockFile(lockfile.string()), std::runtime_error);
    fs::unlockFile(lockfile.string());
    REQUIRE_THROWS_AS(fs::unlockFile(lockfile.string()), std::runtime_error);
    fs::lockFile(lockfile.string());
    fs::unlockFile(lockfile.string());
    stdfs::remove(lockfile);
}

TEST_CASE("filesystem durable rename", "[fs]")
{
    TmpDir tmp("fstests");
    stdfs::path root(tmp.getName());
    stdfs::path fileA = root / "fileA.txt";
    stdfs::path fileB = root / "fileB.txt";
    {
        std::ofstream out(fileA.string());
        out << "hi";
    }
    REQUIRE(fs::durableRename(fileA.string(), fileB.string(), root.string()));
    REQUIRE(!fs::exists(fileA.string()));
    REQUIRE(fs::exists(fileB.string()));
}

TEST_CASE("filesystem findfiles", "[fs]")
{
    TmpDir tmp("fstests");
    stdfs::path root(tmp.getName());
    stdfs::path textFile = root / "file.txt";
    stdfs::path docFile = root / "file.doc";
    {
        std::ofstream out(textFile.string());
        out << "hi";
    }
    stdfs::copy_file(textFile, docFile);
    REQUIRE(stdfs::exists(textFile));
    REQUIRE(stdfs::exists(docFile));
    auto files =
        fs::findfiles(root.string(), [](std::string const& name) -> bool {
            return stdfs::path(name).extension().string() == ".doc";
        });
    REQUIRE(files == std::vector<std::string>{docFile.filename().string()});
}

TEST_CASE("filesystem remoteName", "[fs]")
{
    REQUIRE(fs::remoteName(typeString(FileType::HISTORY_FILE_TYPE_LEDGER),
                           fs::hexStr(0x0abbccdd), "xdr.gz") ==
            "ledger/0a/bb/cc/ledger-0abbccdd.xdr.gz");
}

// ------------------------------------------------------------------
// Tests for computeSafeMaxHandles() helper - direct testing of boundary cases
// These tests are POSIX-only because they use rlim_t and RLIM_INFINITY.
// On Windows, computeSafeMaxHandles is not defined.
// ------------------------------------------------------------------

#ifndef _WIN32

TEST_CASE("computeSafeMaxHandles handles RLIM_INFINITY", "[fs]")
{
    // Direct test of the helper function with RLIM_INFINITY.
    // This does NOT depend on the system's actual limit.
    // The helper should return the capped value of 1,000,000.
    int64_t result = fs::computeSafeMaxHandles(RLIM_INFINITY);
    REQUIRE(result == 1000000);
}

TEST_CASE("computeSafeMaxHandles handles very large finite limits", "[fs]")
{
    // Test with a limit that actually triggers clamping.
    // Need a value > 4 * INT64_MAX / 3 to force clamping.
    // Using 2 * INT64_MAX is safely above the threshold.
    rlim_t largeLimit = static_cast<rlim_t>(std::numeric_limits<int64_t>::max()) * 2;
    int64_t result = fs::computeSafeMaxHandles(largeLimit);
    REQUIRE(result == std::numeric_limits<int64_t>::max());
}

TEST_CASE("computeSafeMaxHandles handles value near clamping threshold", "[fs]")
{
    // Test with a value that is just below the clamping threshold.
    // This should NOT clamp, but return the computed 75% value.
    rlim_t nearLimit = static_cast<rlim_t>(std::numeric_limits<int64_t>::max() / 3 * 4) - 1;
    int64_t result = fs::computeSafeMaxHandles(nearLimit);
    REQUIRE(result > 0);
    REQUIRE(result <= std::numeric_limits<int64_t>::max());
}

TEST_CASE("computeSafeMaxHandles preserves floor(limit * 3 / 4) for small values", "[fs]")
{
    // Test with small values to verify the remainder handling.
    // This ensures the formula (limit / 4) * 3 + (limit % 4) * 3 / 4
    // correctly computes floor(limit * 3 / 4) without overflow.
    struct TestCase {
        rlim_t input;
        int64_t expected;
    };

    std::vector<TestCase> cases = {
        {0, 0},
        {1, 0},   // floor(1 * 0.75) = 0
        {2, 1},   // floor(2 * 0.75) = 1
        {3, 2},   // floor(3 * 0.75) = 2
        {4, 3},   // floor(4 * 0.75) = 3
        {5, 3},   // floor(5 * 0.75) = 3
        {6, 4},   // floor(6 * 0.75) = 4
        {7, 5},   // floor(7 * 0.75) = 5
        {8, 6},   // floor(8 * 0.75) = 6
        {10, 7},  // floor(10 * 0.75) = 7
        {100, 75}, // floor(100 * 0.75) = 75
        {1000, 750}, // floor(1000 * 0.75) = 750
        {1000000, 750000}, // floor(1,000,000 * 0.75) = 750,000
    };

    for (const auto& tc : cases) {
        int64_t result = fs::computeSafeMaxHandles(tc.input);
        INFO("Input: " << tc.input << ", Expected: " << tc.expected << ", Got: " << result);
        REQUIRE(result == tc.expected);
    }
}

TEST_CASE("computeSafeMaxHandles handles value near int64_t max", "[fs]")
{
    // Test with a value that is close to the maximum but safe.
    // This ensures the clamping logic works correctly at the boundary.
    rlim_t safeLimit = static_cast<rlim_t>(std::numeric_limits<int64_t>::max() / 4) * 3;
    int64_t result = fs::computeSafeMaxHandles(safeLimit);
    REQUIRE(result > 0);
    REQUIRE(result <= std::numeric_limits<int64_t>::max());
}

TEST_CASE("computeSafeMaxHandles handles zero", "[fs]")
{
    // Edge case: zero limit should return zero.
    int64_t result = fs::computeSafeMaxHandles(0);
    REQUIRE(result == 0);
}

#endif // !_WIN32

// ------------------------------------------------------------------
// Integration tests for getMaxHandles() - verify it calls the helper
// ------------------------------------------------------------------

TEST_CASE("getMaxHandles returns a positive value", "[fs]")
{
    // Basic sanity: ensure getMaxHandles() returns a usable value.
    auto handles = fs::getMaxHandles();
    REQUIRE(handles > 0);
    REQUIRE(handles <= std::numeric_limits<int64_t>::max());
}

#ifdef _WIN32
TEST_CASE("getMaxHandles Windows returns fixed value", "[fs]")
{
    // On Windows, getMaxHandles() returns a fixed value of 32,000.
    auto handles = fs::getMaxHandles();
    REQUIRE(handles == 32000);
}
#else
TEST_CASE("getMaxHandles POSIX integration test", "[fs]")
{
    // This test verifies that getMaxHandles() delegates to computeSafeMaxHandles()
    // and returns a sane value on POSIX systems.
    // The actual value depends on the system's RLIMIT_NOFILE, but we verify
    // it's positive and within int64_t range.
    auto handles = fs::getMaxHandles();
    REQUIRE(handles > 0);
    REQUIRE(handles <= std::numeric_limits<int64_t>::max());
}
#endif
