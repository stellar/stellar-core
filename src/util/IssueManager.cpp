// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "IssueManager.h"

#include <array>

namespace stellar
{

std::string getIssueHuman(Issue issue)
{
    static const auto issueStrings = std::array<const char*, static_cast<int>(Issue::NUM)>{
        "Local time is not synchronized with NTP time."
    };
    return std::string(issueStrings[static_cast<int>(issue)]);
}

IssueManager::IssueManager()
{
}

IssueManager::~IssueManager()
{
}

void IssueManager::addIssue(Issue issue)
{
    mIssues.insert(issue);
}

void IssueManager::removeIssue(Issue issue)
{
    mIssues.erase(issue);
}

}
