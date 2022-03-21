// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/catch.hpp"
#include "main/Application.h"
#include "test/TestAccount.h"
#include "test/TestExceptions.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/TransactionUtils.h"

using namespace stellar;
using namespace stellar::txtest;

TEST_CASE("invoke contract zig", "[tx][contract]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    auto root = TestAccount::createRoot(*app);
    int64_t contract = 1;
    root.addWasmContract(
        contract, "src/transactions/test/wasm-zig/test_invoke_contract.wasm");
    SCVal arg;
    arg.type(SCV_I32);
    arg.i32() = 5;
    SCVal exp = arg;
    exp.i32() += 1;
    for (int i = 0; i < 3; ++i)
    {
        SCVal res = root.invokeWasmContract(root.getPublicKey(), contract,
                                            "invoke", arg);
        REQUIRE(res == exp);
    }
}
