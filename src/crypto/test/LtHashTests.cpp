// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/Hex.h"
#include "crypto/LtHash.h"
#include "crypto/SHA.h"
#include "test/Catch2.h"
#include "util/Math.h"
#include <algorithm>
#include <chrono>
#include <fmt/format.h>
#include <string>
#include <vector>

using namespace stellar;

namespace
{
std::vector<std::string>
members(size_t n, std::string const& tag)
{
    std::vector<std::string> res;
    for (size_t i = 0; i < n; ++i)
    {
        // Different lengths, the empty string among them.
        res.emplace_back(
            i == 0 ? std::string()
                   : fmt::format("{} {} {}", tag, i, std::string(i % 97, 'x')));
    }
    return res;
}

LtHash16
hashOf(std::vector<std::string> const& ms)
{
    LtHash16 h;
    for (auto const& m : ms)
    {
        h.add(m);
    }
    return h;
}
}

TEST_CASE("lthash does not depend on order or on how the members are split",
          "[crypto][lthash]")
{
    auto ms = members(300, "member");
    auto whole = hashOf(ms);
    REQUIRE(!whole.isZero());
    REQUIRE(LtHash16().isZero());

    // Any order.
    for (int round = 0; round < 5; ++round)
    {
        stellar::shuffle(ms.begin(), ms.end(), getGlobalRandomEngine());
        REQUIRE(hashOf(ms) == whole);
        REQUIRE(hashOf(ms).digest() == whole.digest());
    }

    // Any split into parts, summed in any order.
    for (size_t parts : {1u, 2u, 3u, 7u, 16u, 300u})
    {
        std::vector<LtHash16> partial(parts);
        for (size_t i = 0; i < ms.size(); ++i)
        {
            partial[(i * 31 + 7) % parts].add(ms[i]);
        }
        stellar::shuffle(partial.begin(), partial.end(),
                         getGlobalRandomEngine());
        LtHash16 sum;
        for (auto const& p : partial)
        {
            sum += p;
        }
        REQUIRE(sum == whole);
    }
}

TEST_CASE("lthash tells collections apart and counts repeated members",
          "[crypto][lthash]")
{
    auto ms = members(50, "member");
    auto whole = hashOf(ms);

    // One member more, one less, one changed.
    auto more = ms;
    more.emplace_back("one more");
    REQUIRE(hashOf(more) != whole);
    auto fewer = ms;
    fewer.pop_back();
    REQUIRE(hashOf(fewer) != whole);
    auto changed = ms;
    changed[17] += "!";
    REQUIRE(hashOf(changed) != whole);
    REQUIRE(hashOf(changed).digest() != whole.digest());

    // A member twice is not the member once, and not nothing (as it would be
    // under xor).
    LtHash16 once, twice;
    once.add(std::string("a"));
    twice.add(std::string("a"));
    twice.add(std::string("a"));
    REQUIRE(twice != once);
    REQUIRE(!twice.isZero());

    // Concatenation is not confused with two members.
    LtHash16 ab, a_b;
    ab.add(std::string("ab"));
    a_b.add(std::string("a"));
    a_b.add(std::string("b"));
    REQUIRE(ab != a_b);
}

TEST_CASE("lthash members can be taken away again", "[crypto][lthash]")
{
    auto ms = members(100, "member");
    auto whole = hashOf(ms);

    // Replace a member: take the old one away, add the new one.
    auto replaced = ms;
    replaced[42] = "the new value";
    LtHash16 h = whole;
    h.remove(ms[42]);
    h.add(replaced[42]);
    REQUIRE(h == hashOf(replaced));

    // Take a whole part away.
    std::vector<std::string> first(ms.begin(), ms.begin() + 30);
    std::vector<std::string> rest(ms.begin() + 30, ms.end());
    LtHash16 g = whole;
    g -= hashOf(first);
    REQUIRE(g == hashOf(rest));
    g -= hashOf(rest);
    REQUIRE(g.isZero());
    REQUIRE(g == LtHash16());
}

TEST_CASE("lthash state round-trips and has a fixed value", "[crypto][lthash]")
{
    auto whole = hashOf(members(10, "member"));
    REQUIRE(LtHash16::fromBytes(whole.toBytes()) == whole);

    // The construction is part of what nodes must agree on: pin it. The
    // digest of the empty collection is the SHA-256 of 2048 zero bytes.
    std::array<uint8_t, LtHash16::STATE_BYTES> zeros;
    zeros.fill(0);
    REQUIRE(LtHash16().digest() ==
            sha256(ByteSlice(zeros.data(), zeros.size())));
    LtHash16 abc;
    abc.add(std::string("abc"));
    INFO(binToHex(abc.digest()));
    auto bytes = abc.toBytes();
    INFO(binToHex(ByteSlice(bytes.data(), 16)));
    // (From the first run of this test.)
    REQUIRE(binToHex(abc.digest()) ==
            "2ec21e9619daeb2924bb88d36566dddfd8f4e45506f36eac8bb5d364dc373699");
    REQUIRE(binToHex(ByteSlice(bytes.data(), 16)) ==
            "0ecbf29822e9373da329597a8dcf31ad");
}

TEST_CASE("lthash cost per member", "[.][lthashbench]")
{
    std::vector<std::string> ms;
    for (size_t i = 0; i < 30000; ++i)
    {
        ms.emplace_back(fmt::format("{:0164}", i));
    }
    auto start = std::chrono::steady_clock::now();
    auto h = hashOf(ms);
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now() - start)
                  .count();
    WARN(fmt::format("lthash16: {} members of 164 bytes in {:.1f} ms, {:.0f} "
                     "ns per member",
                     ms.size(), ns / 1e6, double(ns) / ms.size()));
    REQUIRE(!h.isZero());
}
