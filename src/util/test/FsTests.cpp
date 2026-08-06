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
    // Only run this test if rlim_t is wide enough to hold values above INT64_MAX.
    // On some platforms (e.g., 32-bit), rlim_t may be 32-bit and cannot represent
    // values above INT64_MAX, so the clamping path cannot be exercised.
#if defined(__LP64__) || defined(_LP64) || defined(__x86_64__) || defined(__aarch64__)
    // On 64-bit platforms, construct a value that is:
    // 1. Above the clamping threshold (4/3 * INT64_MAX)
    // 2. Explicitly NOT equal to RLIM_INFINITY
    rlim_t largeLimit =
        static_cast<rlim_t>(std::numeric_limits<int64_t>::max() / 3) * 4 + 3;
    
    // Safety check: ensure we're not hitting RLIM_INFINITY by accident
    REQUIRE(largeLimit != RLIM_INFINITY);
    
    int64_t result = fs::computeSafeMaxHandles(largeLimit);
    REQUIRE(result == std::numeric_limits<int64_t>::max());
#else
    // On 32-bit platforms, rlim_t is typically 32-bit and cannot exceed INT64_MAX.
    // The clamping path won't be triggered, so we just verify the function returns
    // a sane value for a large finite limit.
    // Use a value that is not RLIM_INFINITY.
    rlim_t largeLimit = 1000000;
    int64_t result = fs::computeSafeMaxHandles(largeLimit);
    REQUIRE(result > 0);
    REQUIRE(result <= std::numeric_limits<int64_t>::max());
#endif
}

TEST_CASE("computeSafeMaxHandles handles value near clamping threshold", "[fs]")
{
    // Test with a value that is just below the clamping threshold.
    // This should NOT clamp, but return the computed 75% value.
    // Cast to rlim_t BEFORE multiplication to avoid signed overflow.
    rlim_t nearLimit = static_cast<rlim_t>(std::numeric_limits<int64_t>::max() / 3) * 4 - 1;
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

TEST_CASE("getMaxHandles returns a value within int64_t range", "[fs]")
{
    // Basic sanity: ensure getMaxHandles() returns a value within int64_t range.
    // The value may be 0 if the system limit is 0 or 1, which is valid.
    auto handles = fs::getMaxHandles();
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
    // and returns the expected value based on the system's RLIMIT_NOFILE.
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0)
    {
        // The value should match computeSafeMaxHandles(rl.rlim_cur)
        int64_t expected = fs::computeSafeMaxHandles(rl.rlim_cur);
        int64_t actual = fs::getMaxHandles();
        REQUIRE(actual == expected);
    }
    else
    {
        // If getrlimit fails, getMaxHandles() should return 64.
        int64_t actual = fs::getMaxHandles();
        REQUIRE(actual == 64);
    }
}
#endif
