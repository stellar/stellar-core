// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/StatusManager.h"
#include "lib/catch.hpp"

using namespace stellar;

TEST_CASE("status empty on start", "[status-manager]")
{
    StatusManager statusManager;

    REQUIRE(statusManager.begin() == statusManager.end());
    REQUIRE(statusManager.size() == 0);
    REQUIRE(statusManager.getStatusMessage(StatusCategory::NTP).empty());
    REQUIRE(statusManager.getStatusMessage(StatusCategory::HISTORY_CATCHUP)
                .empty());
}

TEST_CASE("status add", "[status-manager]")
{
    StatusManager statusManager;

    statusManager.setStatusMessage(StatusCategory::NTP, "message");

    REQUIRE(statusManager.begin() != statusManager.end());
    REQUIRE(statusManager.size() == 1);
    REQUIRE(statusManager.getStatusMessage(StatusCategory::NTP) == "message");
}

TEST_CASE("status add two different", "[status-manager]")
{
    StatusManager statusManager;

    statusManager.setStatusMessage(StatusCategory::NTP, "ntp");
    statusManager.setStatusMessage(StatusCategory::HISTORY_CATCHUP, "history");

    REQUIRE(statusManager.begin() != statusManager.end());
    REQUIRE(statusManager.size() == 2);
    REQUIRE(statusManager.getStatusMessage(StatusCategory::NTP) == "ntp");
    REQUIRE(statusManager.getStatusMessage(StatusCategory::HISTORY_CATCHUP) ==
            "history");
}

TEST_CASE("status should be replaced", "[status-manager]")
{
    StatusManager statusManager;

    statusManager.setStatusMessage(StatusCategory::NTP, "message 1");
    statusManager.setStatusMessage(StatusCategory::NTP, "message 2");

    REQUIRE(statusManager.begin() != statusManager.end());
    REQUIRE(statusManager.size() == 1);
    REQUIRE(statusManager.getStatusMessage(StatusCategory::NTP) == "message 2");
}

TEST_CASE("status remove", "[status-manager]")
{
    StatusManager statusManager;

    statusManager.setStatusMessage(StatusCategory::NTP, "message");
    statusManager.removeStatusMessage(StatusCategory::NTP);

    REQUIRE(statusManager.begin() == statusManager.end());
    REQUIRE(statusManager.size() == 0);
}
