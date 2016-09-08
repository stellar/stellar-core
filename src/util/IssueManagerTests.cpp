// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/catch.hpp"
#include "util/IssueManager.h"

using namespace stellar;

TEST_CASE("issues empty on start", "[issue-manager]")
{
    IssueManager issueManager;

    REQUIRE(issueManager.begin() == issueManager.end());
    REQUIRE(issueManager.size() == 0);
}

TEST_CASE("issues add", "[issue-manager]")
{
    IssueManager issueManager;

    issueManager.addIssue(Issue::TIME_NOT_SYNCHRONIZED);

    REQUIRE(issueManager.begin() != issueManager.end());
    REQUIRE(issueManager.size() == 1);
    REQUIRE(*issueManager.begin() == Issue::TIME_NOT_SYNCHRONIZED);
}

TEST_CASE("issues do not duplicate", "[issue-manager]")
{
    IssueManager issueManager;

    issueManager.addIssue(Issue::TIME_NOT_SYNCHRONIZED);
    issueManager.addIssue(Issue::TIME_NOT_SYNCHRONIZED);

    REQUIRE(issueManager.begin() != issueManager.end());
    REQUIRE(issueManager.size() == 1);
    REQUIRE(*issueManager.begin() == Issue::TIME_NOT_SYNCHRONIZED);
}

TEST_CASE("issues remove", "[issue-manager]")
{
    IssueManager issueManager;

    issueManager.addIssue(Issue::TIME_NOT_SYNCHRONIZED);
    issueManager.removeIssue(Issue::TIME_NOT_SYNCHRONIZED);

    REQUIRE(issueManager.begin() == issueManager.end());
    REQUIRE(issueManager.size() == 0);
}
