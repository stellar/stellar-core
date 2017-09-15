// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"

#include "crypto/Hex.h"
#include "herder/TxSetFrame.h"
#include "invariant/Invariant.h"
#include "invariant/InvariantDoesNotHold.h"
#include "invariant/InvariantManager.h"
#include "main/Application.h"
#include "ledger/LedgerDelta.h"
#include "lib/catch.hpp"
#include "test/test.h"
#include "util/Timer.h"

using namespace stellar;

namespace InvariantTests
{

class TestInvariant : public Invariant
{
  public:
    TestInvariant(bool shouldFail)
        : mShouldFail(shouldFail)
    {
    }

    virtual std::string getName() const override
    {
        return mShouldFail ? "TestInvariant(Fail)" : "TestInvariant(Succeed)";
    }

    virtual std::string checkOnLedgerClose(LedgerDelta const& delta) override
    {
        return mShouldFail ? "fail" : "";
    }

  private:
    bool mShouldFail;
};
}

using namespace InvariantTests;

TEST_CASE("no duplicate register", "[invariant]")
{
    VirtualClock clock;
    Config cfg = getTestConfig();
    Application::pointer app = Application::create(clock, cfg);

    app->getInvariantManager().registerInvariant<TestInvariant>(true);
    REQUIRE_THROWS_AS(
            app->getInvariantManager().registerInvariant<TestInvariant>(true),
            std::runtime_error);
}

TEST_CASE("no duplicate enable", "[invariant]")
{
    VirtualClock clock;
    Config cfg = getTestConfig();
    Application::pointer app = Application::create(clock, cfg);

    app->getInvariantManager().registerInvariant<TestInvariant>(true);
    app->getInvariantManager().enableInvariant("TestInvariant(Fail)");
    REQUIRE_THROWS_AS(
            app->getInvariantManager().enableInvariant("TestInvariant(Fail)"),
            std::runtime_error);
}

TEST_CASE("only enable registered invariants", "[invariant]")
{
    VirtualClock clock;
    Config cfg = getTestConfig();
    Application::pointer app = Application::create(clock, cfg);

    app->getInvariantManager().registerInvariant<TestInvariant>(true);
    app->getInvariantManager().enableInvariant("TestInvariant(Fail)");
    REQUIRE_THROWS_AS(
            app->getInvariantManager().enableInvariant("WrongName"),
            std::runtime_error);
}

TEST_CASE("onLedgerClose fail/succeed", "[invariant]")
{
    {
        VirtualClock clock;
        Config cfg = getTestConfig();
        Application::pointer app = Application::create(clock, cfg);

        app->getInvariantManager().registerInvariant<TestInvariant>(true);
        app->getInvariantManager().enableInvariant("TestInvariant(Fail)");

        LedgerHeader lh{};
        LedgerDelta ld(lh, app->getDatabase());
        auto tsfp = std::make_shared<TxSetFrame>(hexToBin256(
            "0000000000000000000000000000000000000000000000000000000000000000"));

        REQUIRE_THROWS_AS(
                app->getInvariantManager().checkOnLedgerClose(tsfp, ld),
                InvariantDoesNotHold);
    }

    {
        VirtualClock clock;
        Config cfg = getTestConfig();
        Application::pointer app = Application::create(clock, cfg);

        app->getInvariantManager().registerInvariant<TestInvariant>(false);
        app->getInvariantManager().enableInvariant("TestInvariant(Succeed)");

        LedgerHeader lh{};
        LedgerDelta ld(lh, app->getDatabase());
        auto tsfp = std::make_shared<TxSetFrame>(hexToBin256(
            "0000000000000000000000000000000000000000000000000000000000000000"));

        REQUIRE_NOTHROW(
                app->getInvariantManager().checkOnLedgerClose(tsfp, ld));
    }
}
