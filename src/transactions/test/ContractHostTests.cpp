// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxn.h"
#include "lib/catch.hpp"
#include "test/test.h"
#include <autocheck/autocheck.hpp>
#include <fmt/format.h>
#include <limits>
#include <type_traits>
#include <variant>

#include "transactions/contracts/HostContext.h"
#include "transactions/contracts/HostFunctions.h"
#include "transactions/contracts/HostVal.h"
#include "xdr/Stellar-contract.h"

using namespace stellar;

bool
check_roundtrip_i64(int64_t x)
{
    LOG_DEBUG(DEFAULT_LOG, "random round-trip i64: {}", x);
    if (x >= 0)
    {
        auto hv = HostVal::fromPositiveInt64(x);
        auto y = hv.asU63();
        LOG_DEBUG(DEFAULT_LOG, "got i64: {}", y);
        CHECK(x == y);
        return x == y;
    }
    else
    {
        HostContext ctx;
        auto obj = std::make_unique<SCObject>();
        obj->type(stellar::SCO_I64);
        obj->i64() = x;
        auto hv = HostVal::fromObject<int64_t>(ctx.xdrToHost(obj));
        LOG_DEBUG(DEFAULT_LOG, "got val payload 0x{:x}, object {}",
                  hv.payload(), hv.asObject());
        auto const& obj2 = ctx.getObject(hv);
        REQUIRE(obj2);
        REQUIRE(std::holds_alternative<int64_t>(*obj2));
        auto y = std::get<int64_t>(*obj2);
        LOG_DEBUG(DEFAULT_LOG, "retrieved i64 from object: {}", y);
        CHECK(x == y);
        return x == y;
    }
}

bool
check_roundtrip_i32(int32_t x)
{
    LOG_DEBUG(DEFAULT_LOG, "random round-trip i32: {}", x);
    auto hv = HostVal::fromI32(x);
    auto y = hv.asI32();
    LOG_DEBUG(DEFAULT_LOG, "got i32: {}", y);
    CHECK(x == y);
    return x == y;
}

bool
check_roundtrip_u32(uint32_t x)
{
    LOG_DEBUG(DEFAULT_LOG, "random round-trip u32: {}", x);
    auto hv = HostVal::fromU32(x);
    auto y = hv.asU32();
    LOG_DEBUG(DEFAULT_LOG, "got u32: {}", y);
    CHECK(x == y);
    return x == y;
}

bool
check_roundtrip_u64(int64_t x)
{
    LOG_DEBUG(DEFAULT_LOG, "random round-trip u64: {}", x);
    HostContext ctx;
    auto obj = std::make_unique<SCObject>();
    obj->type(stellar::SCO_U64);
    obj->u64() = x;
    auto hv = HostVal::fromObject<uint64_t>(ctx.xdrToHost(obj));
    LOG_DEBUG(DEFAULT_LOG, "got val payload 0x{:x}, object {}", hv.payload(),
              hv.asObject());
    auto const& obj2 = ctx.getObject(hv);
    REQUIRE(obj2);
    REQUIRE(std::holds_alternative<uint64_t>(*obj2));
    auto y = std::get<uint64_t>(*obj2);
    LOG_DEBUG(DEFAULT_LOG, "retrieved u64 from object: {}", y);
    CHECK(x == y);
    return x == y;
}

bool
check_roundtrip_symbol(std::string const& x)
{
    LOG_DEBUG(DEFAULT_LOG, "random round-trip symbol: {}", x);
    auto hv = HostVal::fromSymbol(x);
    auto y = hv.asSymbol();
    LOG_DEBUG(DEFAULT_LOG, "got symbol: {}", y);
    CHECK(x == y);
    return x == y;
}

template <typename T>
std::vector<T>
boundary_range()
{
    std::vector<T> tmp;
    tmp.push_back(std::numeric_limits<T>::min());
    tmp.push_back(std::numeric_limits<T>::min() + 1);
    tmp.push_back(std::numeric_limits<T>::min() + 2);
    tmp.push_back(std::numeric_limits<T>::min() + 3);
    if constexpr (std::is_signed<T>())
    {
        tmp.push_back(T(-3));
        tmp.push_back(T(-2));
        tmp.push_back(T(-1));
    }
    tmp.push_back(T(0));
    tmp.push_back(T(1));
    tmp.push_back(T(2));
    tmp.push_back(T(3));
    tmp.push_back(std::numeric_limits<T>::max() - 3);
    tmp.push_back(std::numeric_limits<T>::max() - 2);
    tmp.push_back(std::numeric_limits<T>::max() - 1);
    tmp.push_back(std::numeric_limits<T>::max());
    return tmp;
}

TEST_CASE("HostVal tests", "[contracthost]")
{
    autocheck::check<int64_t>(check_roundtrip_i64, 100);
    for (int64_t i : boundary_range<int64_t>())
    {
        check_roundtrip_i64(i);
    }

    autocheck::check<uint64_t>(check_roundtrip_u64, 100);
    for (uint64_t i : boundary_range<uint64_t>())
    {
        check_roundtrip_u64(i);
    }

    autocheck::check<int32_t>(check_roundtrip_i32, 100);
    for (int32_t i : boundary_range<int32_t>())
    {
        check_roundtrip_i32(i);
    }

    autocheck::check<uint32_t>(check_roundtrip_u32, 100);
    for (uint32_t i : boundary_range<uint32_t>())
    {
        check_roundtrip_u32(i);
    }

    std::vector<std::string> stmp{"", "a", "hello", "hi_there", "1234567890"};
    for (auto const& s : stmp)
    {
        check_roundtrip_symbol(s);
    }
}

// A minimal WASM module that calls through to an import
// testHostFunction(i64)->i64 and adds 1 to the return value.
//
// Generated from the following wat:
//
// (module
//   (type (func (param i64) (result i64)))
//   (import "env" "testHostFunction" (func (type 0)))
//   (export "invoke" (func $invoke))
//   (func $invoke (type 0) (param i64) (result i64)
//     local.get 0
//     call 0
//     i64.const 1
//     i64.add))

std::vector<uint8_t> const callTestHostFunction_wasm{
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x06, 0x01, 0x60,
    0x01, 0x7e, 0x01, 0x7e, 0x02, 0x18, 0x01, 0x03, 0x65, 0x6e, 0x76, 0x10,
    0x74, 0x65, 0x73, 0x74, 0x48, 0x6f, 0x73, 0x74, 0x46, 0x75, 0x6e, 0x63,
    0x74, 0x69, 0x6f, 0x6e, 0x00, 0x00, 0x03, 0x02, 0x01, 0x00, 0x07, 0x0a,
    0x01, 0x06, 0x69, 0x6e, 0x76, 0x6f, 0x6b, 0x65, 0x00, 0x01, 0x0a, 0x0b,
    0x01, 0x09, 0x00, 0x20, 0x00, 0x10, 0x00, 0x42, 0x01, 0x7c, 0x0b};

TEST_CASE("WASM execute and invoke host function", "[contracthost]")
{
    HostContext ctx;
    int64_t x{12345}, y{0};
    ctx.getHostFunctions().mTestFunctionCallback.emplace([&](uint64_t i) {
        HostVal v = HostVal::fromPayload(i);
        y = v.asU63();
        return uint64_t(y) + 1;
    });
    std::vector<HostVal> args{HostVal::fromPositiveInt64(x)};
    auto res =
        ctx.invokeWasmFunction(callTestHostFunction_wasm, "invoke", args);
    REQUIRE(y == x);
    REQUIRE(res.has_value());
    REQUIRE(!(*res).trapped);
    REQUIRE((*res).has_value);
    REQUIRE((*res).value.as<uint64_t>() == y + 2);
}