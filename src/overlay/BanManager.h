#pragma once

// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "xdr/Stellar-types.h"

/**
 * Manages list of banned nodes.
 */

namespace stellar
{

class Application;
class Database;

class BanManager
{
  public:
    static std::unique_ptr<BanManager> create(Application& app);
    static void dropAll(Database& db);

    // Ban given node
    virtual void banNode(NodeID nodeID) = 0;

    // Unban given node
    virtual void unbanNode(NodeID nodeID) = 0;

    // Check if node is banned
    virtual bool isBanned(NodeID nodeID) = 0;

    // List banned nodes
    virtual std::vector<std::string> getBans() = 0;

    virtual ~BanManager()
    {
    }
};
}
