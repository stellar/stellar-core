#pragma once

// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <set>
#include <string>

namespace stellar
{

/*
 * List of possible issues with server setup.
 */
enum class Issue
{
    TIME_NOT_SYNCHRONIZED, // local time is not synchronized with NTP server
    NUM
};

std::string getIssueHuman(Issue issue);

/*
 * Class for managing list of issues found with server setup.
 * This list is used for "issues" array in info json.
 */
class IssueManager
{
  public:
    using storage = std::set<Issue>;
    using const_iterator = storage::const_iterator;

    explicit IssueManager();
    ~IssueManager();

    void addIssue(Issue issue);
    void removeIssue(Issue issue);

    const_iterator begin() const { return mIssues.begin(); }
    const_iterator end() const { return mIssues.end(); }
    std::size_t size() const { return mIssues.size(); }

  private:
    storage mIssues;
};

}
