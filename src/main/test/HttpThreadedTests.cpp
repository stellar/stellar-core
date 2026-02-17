// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/catch.hpp"
#include "lib/httpthreaded/request.hpp"
#include "lib/httpthreaded/request_parser.hpp"

using namespace httpThreaded::server;

namespace
{
// Helper: feed a raw HTTP request string into the parser and return
// the final result_type.
request_parser::result_type
parseRaw(std::string const& raw, request& req)
{
    request_parser parser;
    auto [result, iter] = parser.parse(req, raw.begin(), raw.end());
    return result;
}
}

TEST_CASE("request parser rejects malformed Content-Length", "[http]")
{
    // non-numeric Content-Length
    {
        std::string raw = "POST /test HTTP/1.1\r\n"
                          "Content-Length: garbage\r\n"
                          "\r\n"
                          "body";
        request req;
        auto result = parseRaw(raw, req);
        REQUIRE(result == request_parser::bad);
    }

    // negative Content-Length wraps to huge value
    {
        // stoull("-1") wraps to ULLONG_MAX rather than throwing, so the
        // parser accepts the header but will never accumulate enough body
        // bytes — it stays indeterminate.
        std::string raw = "POST /test HTTP/1.1\r\n"
                          "Content-Length: -1\r\n"
                          "\r\n"
                          "body";
        request req;
        auto result = parseRaw(raw, req);
        REQUIRE(result == request_parser::indeterminate);
    }

    // overflow Content-Length
    {
        std::string raw = "POST /test HTTP/1.1\r\n"
                          "Content-Length: 99999999999999999999999\r\n"
                          "\r\n"
                          "body";
        request req;
        auto result = parseRaw(raw, req);
        REQUIRE(result == request_parser::bad);
    }

    // empty Content-Length
    {
        std::string raw = "POST /test HTTP/1.1\r\n"
                          "Content-Length: \r\n"
                          "\r\n"
                          "body";
        request req;
        auto result = parseRaw(raw, req);
        REQUIRE(result == request_parser::bad);
    }

    // Content-Length zero
    {
        std::string raw = "POST /test HTTP/1.1\r\n"
                          "Content-Length: 0\r\n"
                          "\r\n";
        request req;
        auto result = parseRaw(raw, req);
        REQUIRE(result == request_parser::bad);
    }
}

TEST_CASE("request parser accepts valid POST", "[http]")
{
    std::string raw = "POST /test HTTP/1.1\r\n"
                      "Content-Length: 4\r\n"
                      "\r\n"
                      "body";
    request req;
    auto result = parseRaw(raw, req);
    REQUIRE(result == request_parser::good);
    REQUIRE(req.method == "POST");
    REQUIRE(req.uri == "/test");
    REQUIRE(req.body == "body");
}

TEST_CASE("request parser accepts valid GET", "[http]")
{
    std::string raw = "GET /info HTTP/1.1\r\n"
                      "\r\n";
    request req;
    auto result = parseRaw(raw, req);
    REQUIRE(result == request_parser::good);
    REQUIRE(req.method == "GET");
    REQUIRE(req.uri == "/info");
}
