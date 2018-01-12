// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Decoder.h"

#include <autocheck/autocheck.hpp>
#include <lib/catch.hpp>
#include <map>

using namespace stellar;

namespace
{

auto b32_data = std::map<std::string, std::string>{
    {"", ""},
    {"1", "GE======"},
    {"12", "GEZA===="},
    {"123", "GEZDG==="},
    {"1234", "GEZDGNA="},
    {"12345", "GEZDGNBV"},
    {"123456", "GEZDGNBVGY======"},
    {"1234567", "GEZDGNBVGY3Q===="},
    {"12345678", "GEZDGNBVGY3TQ==="},
    {"123456789", "GEZDGNBVGY3TQOI="},
    {"123456789a", "GEZDGNBVGY3TQOLB"},
    {"123456789ab", "GEZDGNBVGY3TQOLBMI======"},
    {"123456789abc", "GEZDGNBVGY3TQOLBMJRQ===="},
    {"123456789abcd", "GEZDGNBVGY3TQOLBMJRWI==="},
    {"123456789abcde", "GEZDGNBVGY3TQOLBMJRWIZI="},
    {"123456789abcdef", "GEZDGNBVGY3TQOLBMJRWIZLG"}};

auto b64_data = std::map<std::string, std::string>{
    {"", ""},
    {"1", "MQ=="},
    {"12", "MTI="},
    {"123", "MTIz"},
    {"1234", "MTIzNA=="},
    {"12345", "MTIzNDU="},
    {"123456", "MTIzNDU2"},
    {"1234567", "MTIzNDU2Nw=="},
    {"12345678", "MTIzNDU2Nzg="},
    {"123456789", "MTIzNDU2Nzg5"},
    {"123456789a", "MTIzNDU2Nzg5YQ=="},
    {"123456789ab", "MTIzNDU2Nzg5YWI="},
    {"123456789abc", "MTIzNDU2Nzg5YWJj"},
    {"123456789abcd", "MTIzNDU2Nzg5YWJjZA=="},
    {"123456789abcde", "MTIzNDU2Nzg5YWJjZGU="},
    {"123456789abcdef", "MTIzNDU2Nzg5YWJjZGVm"}};
}

TEST_CASE("encode_b32", "[decoder]")
{
    for (auto const& item : b32_data)
    {
        SECTION(item.first)
        {
            REQUIRE(item.second == decoder::encode_b32(item.first));
        }
    }
}

TEST_CASE("encode_b64", "[decoder]")
{
    for (auto const& item : b64_data)
    {
        SECTION(item.first)
        {
            REQUIRE(item.second == decoder::encode_b64(item.first));
        }
    }
}

TEST_CASE("encoded_size32", "[decoder]")
{
    for (auto const& item : b32_data)
    {
        SECTION(item.first)
        {
            REQUIRE(item.second.size() ==
                    decoder::encoded_size32(item.first.size()));
        }
    }
}

TEST_CASE("encoded_size64", "[decoder]")
{
    for (auto const& item : b64_data)
    {
        SECTION(item.first)
        {
            REQUIRE(item.second.size() ==
                    decoder::encoded_size64(item.first.size()));
        }
    }
}

TEST_CASE("decode_b32", "[decoder]")
{
    for (auto const& item : b32_data)
    {
        SECTION(item.second)
        {
            auto out = std::string{};
            decoder::decode_b32(item.second, out);
            REQUIRE(item.first == out);
        }
    }
}

TEST_CASE("decode_b64", "[decoder]")
{
    for (auto const& item : b64_data)
    {
        SECTION(item.second)
        {
            auto out = std::string{};
            decoder::decode_b64(item.second, out);
            REQUIRE(item.first == out);
        }
    }
}

TEST_CASE("decode_b64 with iterators", "[decoder]")
{
    for (auto const& item : b64_data)
    {
        SECTION(item.second)
        {
            auto out = std::string{};
            out.reserve(item.second.size());
            decoder::decode_b64(std::begin(item.second), std::end(item.second),
                                std::back_inserter(out));
            REQUIRE(item.first == out);
        }
    }
}

TEST_CASE("base64 roundtrip", "[decoder]")
{
    autocheck::generator<std::vector<uint8_t>> input;
    // check round trip
    for (int s = 0; s < 100; s++)
    {
        std::vector<uint8_t> in(input(s));
        std::string encoded = decoder::encode_b64(in);
        std::vector<uint8_t> decoded;

        decoder::decode_b64(encoded, decoded);
        REQUIRE(in == decoded);
    }
}
