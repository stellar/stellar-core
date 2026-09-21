// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "history/HistoryArchive.h"
#include "test/Catch2.h"
#include "util/Fs.h"
#include "util/TmpDir.h"
#include <fstream>
#include <json/json.h>

using namespace stellar;

namespace
{
std::string const ZERO_HASH(64, '0');

// Exercise live buckets in both HAS versions, and hot-archive buckets in v2.
struct BucketListCase
{
    unsigned version;
    char const* array;
};
BucketListCase const BUCKET_LIST_CASES[] = {
    {1, "currentBuckets"}, {2, "currentBuckets"}, {2, "hotArchiveBuckets"}};

// Start from real serialized state, then change only the field under test.
// This keeps rejection cases otherwise valid, so a different malformed field
// cannot accidentally make a test pass.
Json::Value
validHAS(unsigned version = 1)
{
    Json::Value json;
    Json::Reader reader;
    REQUIRE(reader.parse(HistoryArchiveState{}.toString(), json));
    json["version"] = version;
    json["currentLedger"] = 63;
    if (version == 2)
    {
        json["networkPassphrase"] = "test network";
        json["hotArchiveBuckets"] = json["currentBuckets"];
    }
    return json;
}

// Every case below exercises both fromString() and load(). Accepted input must
// preserve its ledger and bucket counts and survive a serialization round trip;
// rejected input must throw std::runtime_error through either entry point.
void
checkHAS(std::string const& json, bool valid)
{
    Json::Value expected;
    if (valid)
    {
        REQUIRE(Json::Reader{}.parse(json, expected));
    }
    TmpDir dir("has-format");
    auto filename = dir.getName() + "/state.json";
    {
        std::ofstream out(filename, std::ios::binary);
        out.write(json.data(), json.size());
    }
    for (bool fromFile : {false, true})
    {
        INFO("fromFile=" << fromFile);
        HistoryArchiveState state;
        auto load = [&] {
            if (fromFile)
            {
                state.load(filename);
            }
            else
            {
                state.fromString(json);
            }
        };
        if (valid)
        {
            REQUIRE_NOTHROW(load());
            CHECK(state.version == expected["version"].asUInt());
            CHECK(state.currentLedger == expected["currentLedger"].asUInt());
            CHECK(state.networkPassphrase ==
                  expected.get("networkPassphrase", "").asString());
            CHECK(state.currentBuckets.size() == LiveBucketList::kNumLevels);
            if (state.version == 2)
            {
                CHECK(state.hotArchiveBuckets.size() ==
                      HotArchiveBucketList::kNumLevels);
            }
            HistoryArchiveState roundtrip;
            roundtrip.fromString(state.toString());
            CHECK(roundtrip.toString() == state.toString());
        }
        else
        {
            REQUIRE_THROWS_AS(load(), std::runtime_error);
        }
    }
}

void
checkHAS(Json::Value const& json, bool valid)
{
    checkHAS(Json::FastWriter{}.write(json), valid);
}
}

TEST_CASE("HAS accepts supported schemas and ledger numbers",
          "[history][archive-format]")
{
    for (unsigned version : {1, 2})
    {
        INFO("version=" << version);
        auto const base = validHAS(version);
        for (uint32_t ledger : {0u, 1u, 63u, UINT32_MAX})
        {
            auto json = base;
            json["currentLedger"] = ledger;
            checkHAS(json, true); // Range policy belongs to the consumer.
        }
    }
    // v1 supports a passphrase when present, while still accepting its absence.
    auto legacy = validHAS();
    legacy["networkPassphrase"] = "test network";
    checkHAS(legacy, true);
}

TEST_CASE("HAS validates required fields and types",
          "[history][archive-format]")
{
    for (unsigned version : {1, 2})
    {
        INFO("version=" << version);
        auto const base = validHAS(version);
        // v1 permits an absent passphrase and has no hot-archive list.
        // v2 requires both fields in addition to the common fields.
        for (auto field :
             {"version", "server", "currentLedger", "currentBuckets",
              "networkPassphrase", "hotArchiveBuckets"})
        {
            INFO("field=" << field);
            auto json = base;
            json.removeMember(field);
            bool optional =
                version == 1 && (std::string(field) == "networkPassphrase" ||
                                 std::string(field) == "hotArchiveBuckets");
            checkHAS(json, optional);
        }
        for (auto field :
             {"version", "currentLedger", "server", "networkPassphrase",
              "currentBuckets", "hotArchiveBuckets"})
        {
            INFO("null field=" << field);
            auto json = base;
            json[field] = Json::Value();
            checkHAS(json, false);
        }
        // Optional means absent, not present with the wrong JSON type.
        for (Json::Value passphrase :
             {Json::Value(7), Json::Value(true), Json::Value(Json::arrayValue),
              Json::Value(Json::objectValue)})
        {
            auto json = base;
            json["networkPassphrase"] = passphrase;
            checkHAS(json, false);
        }
    }
    // Unsupported versions, negative/overflowing integers, and wrong types.
    for (Json::Value version :
         {Json::Value(0), Json::Value(3), Json::Value(-1),
          Json::Value(UINT32_MAX), Json::Value(Json::UInt64(1) << 32),
          Json::Value(1.5), Json::Value("1"), Json::Value(true)})
    {
        INFO("invalid version=" << version);
        auto json = validHAS();
        json["version"] = version;
        checkHAS(json, false);
    }
    // Ledger numbers have a different contract: every uint32_t is valid at
    // the format layer, but values outside that range and wrong types fail.
    for (Json::Value ledger :
         {Json::Value(-1), Json::Value(Json::UInt64(1) << 32), Json::Value(1.5),
          Json::Value("1"), Json::Value(true)})
    {
        INFO("invalid ledger=" << ledger);
        auto json = validHAS();
        json["currentLedger"] = ledger;
        checkHAS(json, false);
    }
}

TEST_CASE("HAS rejects unused fields and null bucket values",
          "[history][archive-format]")
{
    for (unsigned version : {1, 2})
    {
        INFO("version=" << version);
        auto json = validHAS(version);
        json["unused"] = true;
        checkHAS(json, false);
    }
    // A well-formed hot-archive list is still unused, and invalid, in v1.
    auto legacy = validHAS();
    legacy["hotArchiveBuckets"] = legacy["currentBuckets"];
    checkHAS(legacy, false);

    for (auto const& bucketList : BUCKET_LIST_CASES)
    {
        INFO("version=" << bucketList.version
                        << ", array=" << bucketList.array);
        auto const base = validHAS(bucketList.version);
        auto json = base;
        json[bucketList.array][0] = Json::Value();
        checkHAS(json, false);
        json = base;
        json[bucketList.array][0]["unused"] = true;
        checkHAS(json, false);
        for (auto field : {"curr", "snap", "next"})
        {
            INFO("null level field=" << field);
            json = base;
            json[bucketList.array][0][field] = Json::Value();
            checkHAS(json, false);
        }

        for (unsigned state : {0, 1, 2})
        {
            INFO("future state=" << state);
            auto withFuture = base;
            auto& next = withFuture[bucketList.array][1]["next"];
            next["state"] = state;
            if (state == 1)
            {
                next["output"] = ZERO_HASH;
            }
            else if (state == 2)
            {
                next["curr"] = ZERO_HASH;
                next["snap"] = ZERO_HASH;
                next["shadow"].append(ZERO_HASH);
            }

            // Every field used by this state must be non-null.
            for (auto const& field : next.getMemberNames())
            {
                INFO("null future field=" << field);
                json = withFuture;
                json[bucketList.array][1]["next"][field] = Json::Value();
                checkHAS(json, false);
            }
            // Fields belonging to other future states must not be ignored.
            for (auto field : {"unused", "curr", "snap", "output", "shadow"})
            {
                if (next.isMember(field))
                {
                    continue;
                }
                INFO("unused future field=" << field);
                json = withFuture;
                auto& extra = json[bucketList.array][1]["next"][field];
                if (std::string(field) == "shadow")
                {
                    extra.append(ZERO_HASH);
                }
                else
                {
                    extra = ZERO_HASH;
                }
                checkHAS(json, false);
            }
            if (state == 2)
            {
                json = withFuture;
                json[bucketList.array][1]["next"]["shadow"][0] = Json::Value();
                checkHAS(json, false);
            }
        }
    }
}

TEST_CASE("HAS rejects duplicate object fields", "[history][archive-format]")
{
    for (unsigned version : {1, 2})
    {
        INFO("version=" << version);
        // Duplicate a field at each object depth: HAS, bucket level, future.
        for (std::string const& field :
             {"\"version\":" + std::to_string(version),
              "\"curr\":\"" + ZERO_HASH + "\"", std::string("\"state\":0")})
        {
            INFO("duplicate field=" << field);
            auto json = Json::FastWriter{}.write(validHAS(version));
            auto pos = json.find(field);
            REQUIRE(pos != std::string::npos);
            json.insert(pos, field + ",");
            checkHAS(json, false);
        }
    }
}

TEST_CASE("HAS rejects wrong-sized bucket vectors", "[history][archive-format]")
{
    for (auto const& bucketList : BUCKET_LIST_CASES)
    {
        INFO("version=" << bucketList.version
                        << ", array=" << bucketList.array);
        auto const base = validHAS(bucketList.version);
        // Empty, too short, and the boundaries around the required 11 levels.
        for (unsigned count : {0, 5, 10, 12})
        {
            INFO("count=" << count);
            auto json = base;
            auto& levels = json[bucketList.array];
            levels.resize(count);
            for (unsigned i = 0; i < count; ++i)
            {
                // Keep every element valid, including newly added ones.
                levels[i] = base[bucketList.array][0];
            }
            checkHAS(json, false);
        }
    }
}

TEST_CASE("HAS validates every bucket hash", "[history][archive-format]")
{
    struct HashCase
    {
        char const* description;
        std::string hash;
        bool valid;
    };
    HashCase const hashes[] = {
        {"zero hash", ZERO_HASH, true},
        {"lowercase hex", std::string(64, 'a'), true},
        {"uppercase hex", std::string(64, 'A'), true},
        {"empty", "", false},
        {"short", "aabb", false},
        {"one character short", std::string(63, '0'), false},
        {"one character long", std::string(65, '0'), false},
        {"long", std::string(128, 'a'), false},
        {"non-hex lowercase at the correct length", std::string(64, 'g'),
         false},
        {"non-hex uppercase at the correct length", std::string(64, 'Z'),
         false},
        {"path traversal", "../../etc/passwd", false}};

    // Apply the same hash cases to every hash-bearing field in each schema.
    // inputCurr/inputSnap below name next.curr/next.snap, not level.curr/snap.
    for (auto const& bucketList : BUCKET_LIST_CASES)
    {
        INFO("version=" << bucketList.version
                        << ", array=" << bucketList.array);
        for (auto field :
             {"curr", "snap", "output", "inputCurr", "inputSnap", "shadow"})
        {
            INFO("field=" << field);
            for (auto const& test : hashes)
            {
                INFO("hash case=" << test.description);
                auto json = validHAS(bucketList.version);
                std::string name(field);
                bool const isLevelHash = name == "curr" || name == "snap";
                auto& level = json[bucketList.array][isLevelHash ? 0 : 1];
                Json::Value value(test.hash);
                if (isLevelHash)
                {
                    level[field] = value;
                }
                else if (name == "output")
                {
                    level["next"]["state"] = 1; // FB_HASH_OUTPUT
                    level["next"]["output"] = value;
                }
                else
                {
                    auto& next = level["next"];
                    next["state"] = 2; // FB_HASH_INPUTS
                    next["curr"] =
                        name == "inputCurr" ? value : Json::Value(ZERO_HASH);
                    next["snap"] =
                        name == "inputSnap" ? value : Json::Value(ZERO_HASH);
                    next["shadow"] = Json::Value(Json::arrayValue);
                    next["shadow"].append(
                        name == "shadow" ? value : Json::Value(ZERO_HASH));
                }
                checkHAS(json, test.valid);
            }
        }
    }
}

TEST_CASE("HAS validates future states and shadow counts",
          "[history][archive-format]")
{
    for (auto const& bucketList : BUCKET_LIST_CASES)
    {
        INFO("version=" << bucketList.version
                        << ", array=" << bucketList.array);
        // Only 0 (clear), 1 (hash output), and 2 (hash inputs) are serialized.
        // 3 and 4 are in-memory live states; -1 and 99 are outside the enum.
        for (int state : {-1, 3, 4, 99})
        {
            INFO("invalid state=" << state);
            auto json = validHAS(bucketList.version);
            json[bucketList.array][1]["next"]["state"] = state;
            checkHAS(json, false);
        }
        // Empty and exactly 32 shadows are allowed; 33 exceeds the cap.
        for (unsigned count : {0, 32, 33})
        {
            INFO("shadow count=" << count);
            auto json = validHAS(bucketList.version);
            auto& next = json[bucketList.array][1]["next"];
            next["state"] = 2; // FB_HASH_INPUTS
            next["curr"] = ZERO_HASH;
            next["snap"] = ZERO_HASH;
            next["shadow"] = Json::Value(Json::arrayValue);
            for (unsigned i = 0; i < count; ++i)
            {
                next["shadow"].append(ZERO_HASH);
            }
            checkHAS(json, count <= 32);
            // Test missing fields only on otherwise valid input: 33 shadows
            // would fail regardless of whether the field was required.
            if (count <= 32)
            {
                for (auto field : {"curr", "snap", "shadow"})
                {
                    INFO("missing input field=" << field);
                    auto missing = json;
                    missing[bucketList.array][1]["next"].removeMember(field);
                    checkHAS(missing, false);
                }
            }
        }
        auto json = validHAS(bucketList.version);
        json[bucketList.array][1]["next"]["state"] = 1; // FB_HASH_OUTPUT
        checkHAS(json, false);                          // Missing output.
        json = validHAS(bucketList.version);
        json[bucketList.array][1]["next"].removeMember("state");
        checkHAS(json, false);
    }
}

TEST_CASE("HAS rejects malformed and oversized input",
          "[history][archive-format]")
{
    auto valid = Json::FastWriter{}.write(validHAS());
    for (std::string const& json :
         {std::string{}, std::string("not JSON"), std::string("[]"),
          std::string("[1]"), std::string("null"),
          valid.substr(0, valid.size() / 2), valid + "garbage"})
    {
        checkHAS(json, false);
    }
    // Whitespace padding keeps the JSON valid: rejection above the byte limit
    // must come from the size check, not an unrelated JSON syntax error.
    valid.resize(HistoryArchiveState::MAX_HAS_FILE_SIZE, ' ');
    checkHAS(valid, true);
    valid.push_back(' ');
    checkHAS(valid, false);

    TmpDir dir("has-missing");
    HistoryArchiveState state;
    CHECK_THROWS_AS(state.load(dir.getName() + "/missing"), std::runtime_error);
}

TEST_CASE("hexDir validates its directory prefix", "[history][archive-format]")
{
    CHECK(fs::hexDir("aabbcc") == "aa/bb/cc");
    CHECK(fs::hexDir("aabbccdd0011223344556677889900aabbccdd0011223344556677889"
                     "900aabb") == "aa/bb/cc");
    for (auto const& input : {"", "zz", "not-a-hex-string", "gg0000"})
    {
        CHECK_THROWS_AS(fs::hexDir(input), std::runtime_error);
    }
}
