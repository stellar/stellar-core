// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/catch.hpp"
#include "util/StatusManager.h"

using namespace stellar;

TEST_CASE("status empty on start", "[status-manager]")
{
    StatusManager statusManager;

    REQUIRE(statusManager.begin() == statusManager.end());
    REQUIRE(statusManager.size() == 0);
}

TEST_CASE("status add", "[status-manager]")
{
    StatusManager statusManager;

    statusManager.addStatusMessage(StatusCategory::NTP, "message");

    REQUIRE(statusManager.begin() != statusManager.end());
    REQUIRE(statusManager.size() == 1);
    REQUIRE(statusManager.begin()->first == StatusCategory::NTP);
    REQUIRE(statusManager.begin()->second == "message");
}

TEST_CASE("status should be replaced", "[status-manager]")
{
    StatusManager statusManager;

    statusManager.addStatusMessage(StatusCategory::NTP, "message 1");
    statusManager.addStatusMessage(StatusCategory::NTP, "message 2");

    REQUIRE(statusManager.begin() != statusManager.end());
    REQUIRE(statusManager.size() == 1);
    REQUIRE(statusManager.begin()->first == StatusCategory::NTP);
    REQUIRE(statusManager.begin()->second == "message 2");
}

TEST_CASE("status remove", "[status-manager]")
{
    StatusManager statusManager;

    statusManager.addStatusMessage(StatusCategory::NTP, "message");
    statusManager.removeStatusMessage(StatusCategory::NTP);

    REQUIRE(statusManager.begin() == statusManager.end());
    REQUIRE(statusManager.size() == 0);
}
