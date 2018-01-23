#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Application.h"
#include "xdr/Stellar-types.h"
#include <string>

namespace stellar
{

class ExternalQueue
{
  public:
    ExternalQueue(Application& app);

    static void dropAll(Database& db);

    // checks if a given resource ID is well formed
    static bool validateResourceID(std::string const& resid);

    // sets the cursor of a given resource
    void setCursorForResource(std::string const& resid, uint32 cursor);
    // gets the cursor of a given resource, gets all cursors of resid is empty
    void getCursorForResource(std::string const& resid,
                              std::map<std::string, uint32>& curMap);
    // deletes the subscription for the resource
    void deleteCursor(std::string const& resid);

    // safely delete data, maximum count entries from each table
    void deleteOldEntries(uint32 count);

  private:
    void checkID(std::string const& resid);
    std::string getCursor(std::string const& resid);

    static std::string kSQLCreateStatement;

    Application& mApp;
};
}
