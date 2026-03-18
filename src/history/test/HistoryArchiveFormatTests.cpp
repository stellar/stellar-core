// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// Comprehensive tests for History Archive State (HAS) format validation.
// Ensures that all malformed or crafted HAS JSON inputs are rejected
// gracefully (via exception) rather than crashing.

#include "bucket/HotArchiveBucketList.h"
#include "bucket/LiveBucketList.h"
#include "history/HistoryArchive.h"
#include "main/Application.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Fs.h"
#include <fmt/format.h>
#include <fstream>
#include <functional>
#include <lib/catch.hpp>
#include <limits>
#include <sstream>

using namespace stellar;

namespace
{

std::string const ZERO_HASH(64, '0');

// Per-level bucket fields for the HAS JSON builder.
struct TestBucketLevel
{
    std::string curr = ZERO_HASH;
    std::string snap = ZERO_HASH;
    std::string next = "{\"state\": 0}";
};

// Emit a JSON array of bucket levels.
void
emitBucketArray(std::ostringstream& json,
                std::vector<TestBucketLevel> const& levels)
{
    json << "[\n";
    for (size_t i = 0; i < levels.size(); ++i)
    {
        auto const& l = levels[i];
        json << "    {\"curr\": \"" << l.curr << "\", \"next\": " << l.next
             << ", \"snap\": \"" << l.snap << "\"}";
        if (i + 1 < levels.size())
        {
            json << ",";
        }
        json << "\n";
    }
    json << "  ]";
}

// Core builder: emits a HAS JSON string from explicit parameters.
// Every makeHAS* helper delegates here. For version 2, pass hotLevels
// to include the hotArchiveBuckets and networkPassphrase fields.
std::string
buildHASJson(uint32_t version, uint32_t currentLedger,
             std::vector<TestBucketLevel> const& levels,
             std::vector<TestBucketLevel> const* hotLevels = nullptr)
{
    std::ostringstream json;
    json << "{\n";
    json << "  \"version\": " << version << ",\n";
    json << "  \"server\": \"test\",\n";
    json << "  \"currentLedger\": " << currentLedger << ",\n";
    if (hotLevels)
    {
        json << "  \"networkPassphrase\": \"Test SDF Network ; September "
                "2015\",\n";
    }
    json << "  \"currentBuckets\": ";
    emitBucketArray(json, levels);
    if (hotLevels)
    {
        json << ",\n  \"hotArchiveBuckets\": ";
        emitBucketArray(json, *hotLevels);
    }
    json << "\n}\n";
    return json.str();
}

// Default all-zeros bucket list matching LiveBucketList structure.
std::vector<TestBucketLevel>
defaultLevels()
{
    return std::vector<TestBucketLevel>(LiveBucketList::kNumLevels);
}

// Convenience: valid HAS with optional ledger override.
std::string
makeHASJson(uint32_t currentLedger = 63)
{
    return buildHASJson(1, currentLedger, defaultLevels());
}

// Override a single level's fields via a mutation lambda.
std::string
makeHASJsonWithLevel(int level,
                     std::function<void(TestBucketLevel&)> const& mutate)
{
    auto levels = defaultLevels();
    mutate(levels.at(level));
    return buildHASJson(1, 63, levels);
}

// Override the number of levels.
std::string
makeHASJsonWithLevelCount(int numLevels)
{
    return buildHASJson(1, 63, std::vector<TestBucketLevel>(numLevels));
}

// Build a valid version 2 HAS with hot archive buckets.
std::string
makeHASJsonV2()
{
    auto hot = std::vector<TestBucketLevel>(HotArchiveBucketList::kNumLevels);
    return buildHASJson(2, 63, defaultLevels(), &hot);
}
}

TEST_CASE("valid HAS round-trips through fromString",
          "[history][archive-format]")
{
    auto json = makeHASJson(63);
    HistoryArchiveState has;
    REQUIRE_NOTHROW(has.fromString(json));
    CHECK(has.currentLedger == 63);
    CHECK(has.currentBuckets.size() == LiveBucketList::kNumLevels);
}

TEST_CASE("valid HAS with currentLedger 0", "[history][archive-format]")
{
    auto json = makeHASJson(0);
    HistoryArchiveState has;
    REQUIRE_NOTHROW(has.fromString(json));
    CHECK(has.currentLedger == 0);
}

TEST_CASE("HAS rejects invalid version", "[history][archive-format]")
{
    auto withVersion = [](int v) {
        std::string json = makeHASJson(63);
        auto pos = json.find("\"version\": 1");
        REQUIRE(pos != std::string::npos);
        json.replace(pos, 12, fmt::format("\"version\": {}", v));
        return json;
    };

    // version 0
    {
        HistoryArchiveState has;
        REQUIRE_THROWS_AS(has.fromString(withVersion(0)), std::runtime_error);
    }

    // version 3
    {
        HistoryArchiveState has;
        REQUIRE_THROWS_AS(has.fromString(withVersion(3)), std::runtime_error);
    }
}

TEST_CASE("HAS rejects extreme currentLedger", "[history][archive-format]")
{
    // UINT32_MAX
    {
        auto json = makeHASJson(std::numeric_limits<uint32_t>::max());
        HistoryArchiveState has;
        REQUIRE_THROWS_AS(has.fromString(json), std::runtime_error);
    }

    // Just above MAX_CURRENT_LEDGER
    {
        auto json = makeHASJson(HistoryArchiveState::MAX_CURRENT_LEDGER + 1);
        HistoryArchiveState has;
        REQUIRE_THROWS_AS(has.fromString(json), std::runtime_error);
    }

    // Exactly at MAX_CURRENT_LEDGER (should be accepted)
    {
        auto json = makeHASJson(HistoryArchiveState::MAX_CURRENT_LEDGER);
        HistoryArchiveState has;
        REQUIRE_NOTHROW(has.fromString(json));
    }
}
TEST_CASE("HAS rejects wrong-sized bucket vectors", "[history][archive-format]")
{
    // too many currentBuckets
    {
        auto json = makeHASJsonWithLevelCount(12);
        HistoryArchiveState has;
        REQUIRE_THROWS_AS(has.fromString(json), std::runtime_error);
    }

    // too few currentBuckets
    {
        auto json = makeHASJsonWithLevelCount(5);
        HistoryArchiveState has;
        REQUIRE_THROWS_AS(has.fromString(json), std::runtime_error);
    }

    // zero currentBuckets
    {
        auto json = makeHASJsonWithLevelCount(0);
        HistoryArchiveState has;
        REQUIRE_THROWS_AS(has.fromString(json), std::runtime_error);
    }
}

TEST_CASE("HAS rejects oversized input", "[history][archive-format]")
{
    // oversized string
    {
        std::string oversized(HistoryArchiveState::MAX_HAS_FILE_SIZE + 1, 'x');
        HistoryArchiveState has;
        REQUIRE_THROWS_AS(has.fromString(oversized), std::runtime_error);
    }

    // oversized file
    {
        VirtualClock clock;
        auto cfg = getTestConfig();
        auto app = createTestApplication(clock, cfg);
        auto tmpDir = app->getTmpDirManager().tmpDir("has-test");
        std::string filename = tmpDir.getName() + "/oversized-has.json";

        {
            std::ofstream out(filename);
            std::string padding(HistoryArchiveState::MAX_HAS_FILE_SIZE + 1,
                                ' ');
            out << padding;
        }

        HistoryArchiveState has;
        REQUIRE_THROWS_AS(has.load(filename), std::runtime_error);
    }
}

TEST_CASE("HAS rejects malformed JSON", "[history][archive-format]")
{
    // not JSON at all
    {
        HistoryArchiveState has;
        REQUIRE_THROWS(has.fromString("this is not json"));
    }

    // empty string
    {
        HistoryArchiveState has;
        REQUIRE_THROWS(has.fromString(""));
    }

    // truncated JSON
    {
        auto json = makeHASJson(63);
        // Truncate in the middle
        json = json.substr(0, json.size() / 2);
        HistoryArchiveState has;
        REQUIRE_THROWS(has.fromString(json));
    }

    // missing currentBuckets field
    {
        std::string json = "{\n"
                           "  \"version\": 1,\n"
                           "  \"server\": \"test\",\n"
                           "  \"currentLedger\": 63\n"
                           "}\n";
        HistoryArchiveState has;
        REQUIRE_THROWS(has.fromString(json));
    }

    // missing version field
    {
        std::string json = "{\n"
                           "  \"server\": \"test\",\n"
                           "  \"currentLedger\": 63,\n"
                           "  \"currentBuckets\": []\n"
                           "}\n";
        HistoryArchiveState has;
        REQUIRE_THROWS(has.fromString(json));
    }

    // missing currentLedger field
    {
        std::string json = "{\n"
                           "  \"version\": 1,\n"
                           "  \"server\": \"test\",\n"
                           "  \"currentBuckets\": []\n"
                           "}\n";
        HistoryArchiveState has;
        REQUIRE_THROWS(has.fromString(json));
    }

    // version 2 missing networkPassphrase
    {
        // version 2 requires networkPassphrase; omitting it should fail
        auto hot =
            std::vector<TestBucketLevel>(HotArchiveBucketList::kNumLevels);
        // Build manually without networkPassphrase
        std::ostringstream json;
        json << "{\n";
        json << "  \"version\": 2,\n";
        json << "  \"server\": \"test\",\n";
        json << "  \"currentLedger\": 63,\n";
        json << "  \"currentBuckets\": ";
        emitBucketArray(json, defaultLevels());
        json << ",\n  \"hotArchiveBuckets\": ";
        emitBucketArray(json, hot);
        json << "\n}\n";

        HistoryArchiveState has;
        REQUIRE_THROWS(has.fromString(json.str()));
    }

    // version 2 missing hotArchiveBuckets
    {
        // version 2 expects hotArchiveBuckets; omitting it should fail
        std::ostringstream json;
        json << "{\n";
        json << "  \"version\": 2,\n";
        json << "  \"server\": \"test\",\n";
        json << "  \"currentLedger\": 63,\n";
        json << "  \"networkPassphrase\": \"Test SDF Network ; September "
                "2015\",\n";
        json << "  \"currentBuckets\": ";
        emitBucketArray(json, defaultLevels());
        json << "\n}\n";

        HistoryArchiveState has;
        REQUIRE_THROWS(has.fromString(json.str()));
    }

    // version 1 valid without networkPassphrase
    {
        // version 1 should work fine without networkPassphrase
        auto json = makeHASJson(63);
        HistoryArchiveState has;
        REQUIRE_NOTHROW(has.fromString(json));
    }

    // version 2 valid with all fields
    {
        auto json = makeHASJsonV2();
        HistoryArchiveState has;
        REQUIRE_NOTHROW(has.fromString(json));
    }
}

// ===========================================================================
// FutureBucket state validation
// ===========================================================================

TEST_CASE("FutureBucket rejects invalid state values",
          "[history][archive-format]")
{
    // state 3 (FB_LIVE_OUTPUT - never serialized)
    {
        auto json = makeHASJsonWithLevel(
            1, [](TestBucketLevel& l) { l.next = "{\"state\": 3}"; });
        HistoryArchiveState has;
        REQUIRE_THROWS_AS(has.fromString(json), std::runtime_error);
    }

    // state 4 (FB_LIVE_INPUTS - never serialized)
    {
        auto json = makeHASJsonWithLevel(
            1, [](TestBucketLevel& l) { l.next = "{\"state\": 4}"; });
        HistoryArchiveState has;
        REQUIRE_THROWS_AS(has.fromString(json), std::runtime_error);
    }

    // state 99 (out of range)
    {
        auto json = makeHASJsonWithLevel(
            1, [](TestBucketLevel& l) { l.next = "{\"state\": 99}"; });
        HistoryArchiveState has;
        REQUIRE_THROWS_AS(has.fromString(json), std::runtime_error);
    }
}

TEST_CASE("FutureBucket rejects empty output hash", "[history][archive-format]")
{
    auto json = makeHASJsonWithLevel(1, [](TestBucketLevel& l) {
        l.next = "{\"state\": 1, \"output\": \"\"}";
    });
    HistoryArchiveState has;
    REQUIRE_THROWS_AS(has.fromString(json), std::runtime_error);
}

TEST_CASE("FutureBucket rejects empty input hashes",
          "[history][archive-format]")
{
    // empty curr hash
    {
        auto json = makeHASJsonWithLevel(1, [](TestBucketLevel& l) {
            l.next = "{\"state\": 2, \"curr\": \"\", \"snap\": \"" + ZERO_HASH +
                     "\", \"shadow\": []}";
        });
        HistoryArchiveState has;
        REQUIRE_THROWS_AS(has.fromString(json), std::runtime_error);
    }

    // empty snap hash
    {
        auto json = makeHASJsonWithLevel(1, [](TestBucketLevel& l) {
            l.next = "{\"state\": 2, \"curr\": \"" + ZERO_HASH +
                     "\", \"snap\": \"\", \"shadow\": []}";
        });
        HistoryArchiveState has;
        REQUIRE_THROWS_AS(has.fromString(json), std::runtime_error);
    }
}

TEST_CASE("FutureBucket rejects too many shadow hashes",
          "[history][archive-format]")
{
    // Build a shadow array with 33 entries (just over MAX_SHADOW_HASHES=32)
    std::ostringstream shadows;
    shadows << "[";
    for (int i = 0; i < 33; ++i)
    {
        if (i > 0)
        {
            shadows << ", ";
        }
        shadows << "\"" << ZERO_HASH << "\"";
    }
    shadows << "]";

    auto shadowStr = shadows.str();
    auto json = makeHASJsonWithLevel(1, [&](TestBucketLevel& l) {
        l.next = "{\"state\": 2, \"curr\": \"" + ZERO_HASH +
                 "\", \"snap\": \"" + ZERO_HASH +
                 "\", \"shadow\": " + shadowStr + "}";
    });
    HistoryArchiveState has;
    REQUIRE_THROWS_AS(has.fromString(json), std::runtime_error);
}

// ===========================================================================
// Hash string format validation
// ===========================================================================

TEST_CASE("HAS rejects non-hex curr hash", "[history][archive-format]")
{
    // contains non-hex characters
    {
        std::string badHash =
            "gg00000000000000000000000000000000000000000000000000000000000000";
        auto json = makeHASJsonWithLevel(
            0, [&](TestBucketLevel& l) { l.curr = badHash; });
        HistoryArchiveState has;
        REQUIRE_THROWS_AS(has.fromString(json), std::runtime_error);
    }

    // special characters
    {
        std::string badHash =
            "../../../../../../etc/passwd00000000000000000000000000000000";
        auto json = makeHASJsonWithLevel(
            0, [&](TestBucketLevel& l) { l.curr = badHash; });
        HistoryArchiveState has;
        REQUIRE_THROWS_AS(has.fromString(json), std::runtime_error);
    }
}

TEST_CASE("HAS rejects non-hex snap hash", "[history][archive-format]")
{
    std::string badHash =
        "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz";
    auto json =
        makeHASJsonWithLevel(0, [&](TestBucketLevel& l) { l.snap = badHash; });
    HistoryArchiveState has;
    REQUIRE_THROWS_AS(has.fromString(json), std::runtime_error);
}

TEST_CASE("HAS rejects wrong-length hash strings", "[history][archive-format]")
{
    // too short
    {
        auto json = makeHASJsonWithLevel(
            0, [](TestBucketLevel& l) { l.curr = "aabb"; });
        HistoryArchiveState has;
        REQUIRE_THROWS_AS(has.fromString(json), std::runtime_error);
    }

    // too long
    {
        std::string longHash(128, 'a');
        auto json = makeHASJsonWithLevel(
            0, [&](TestBucketLevel& l) { l.curr = longHash; });
        HistoryArchiveState has;
        REQUIRE_THROWS_AS(has.fromString(json), std::runtime_error);
    }

    // empty hash
    {
        auto json =
            makeHASJsonWithLevel(0, [](TestBucketLevel& l) { l.curr = ""; });
        HistoryArchiveState has;
        REQUIRE_THROWS_AS(has.fromString(json), std::runtime_error);
    }
}

TEST_CASE("HAS rejects non-hex FutureBucket output hash",
          "[history][archive-format]")
{
    std::string badHash =
        "ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ";
    auto json = makeHASJsonWithLevel(1, [&](TestBucketLevel& l) {
        l.next = "{\"state\": 1, \"output\": \"" + badHash + "\"}";
    });
    HistoryArchiveState has;
    REQUIRE_THROWS_AS(has.fromString(json), std::runtime_error);
}

TEST_CASE("HAS rejects non-hex FutureBucket input hashes",
          "[history][archive-format]")
{
    std::string badHash =
        "ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ";

    // bad curr in FB_HASH_INPUTS
    {
        auto json = makeHASJsonWithLevel(1, [&](TestBucketLevel& l) {
            l.next = "{\"state\": 2, \"curr\": \"" + badHash +
                     "\", \"snap\": \"" + ZERO_HASH + "\", \"shadow\": []}";
        });
        HistoryArchiveState has;
        REQUIRE_THROWS_AS(has.fromString(json), std::runtime_error);
    }

    // bad shadow hash
    {
        auto json = makeHASJsonWithLevel(1, [&](TestBucketLevel& l) {
            l.next = "{\"state\": 2, \"curr\": \"" + ZERO_HASH +
                     "\", \"snap\": \"" + ZERO_HASH + "\", \"shadow\": [\"" +
                     badHash + "\"]}";
        });
        HistoryArchiveState has;
        REQUIRE_THROWS_AS(has.fromString(json), std::runtime_error);
    }
}

TEST_CASE("hexDir rejects non-hex strings", "[history][archive-format]")
{
    CHECK_NOTHROW(fs::hexDir("aabbcc"));
    CHECK_NOTHROW(fs::hexDir(
        "aabbccdd0011223344556677889900aabbccdd0011223344556677889900aabb"));

    CHECK_THROWS_AS(fs::hexDir(""), std::runtime_error);
    CHECK_THROWS_AS(fs::hexDir("zz"), std::runtime_error);
    CHECK_THROWS_AS(fs::hexDir("not-a-hex-string"), std::runtime_error);
    CHECK_THROWS_AS(fs::hexDir("gg0000"), std::runtime_error);
}
