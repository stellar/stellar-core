#pragma once

// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <map>
#include <string>

namespace stellar
{

/*
 * List of possible categories of status messages.
 */
enum class StatusCategory
{
    HISTORY_CATCHUP,
    HISTORY_PUBLISH,
    NTP,
    REQUIRES_UPGRADES,
    COUNT
};

/*
 * Class for managing status message of different categories.
 * This list is used for "status" array in info json.
 */
class StatusManager
{
  public:
    using storage = std::map<StatusCategory, std::string>;
    using const_iterator = storage::const_iterator;

    explicit StatusManager();
    ~StatusManager();

    void setStatusMessage(StatusCategory issue, std::string message);
    void removeStatusMessage(StatusCategory issue);
    std::string getStatusMessage(StatusCategory issue) const;

    const_iterator
    begin() const
    {
        return mStatusMessages.begin();
    }
    const_iterator
    end() const
    {
        return mStatusMessages.end();
    }
    std::size_t
    size() const
    {
        return mStatusMessages.size();
    }

  private:
    storage mStatusMessages;
};
}
