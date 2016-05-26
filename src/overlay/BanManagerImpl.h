#pragma once

// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/BanManager.h"

/*
 * Maintain banned set of nodes
 */
namespace stellar
{

class BanManagerImpl : public BanManager
{
  protected:
    Application& mApp;

  public:
    BanManagerImpl(Application& app);
    ~BanManagerImpl();

    void banNode(NodeID nodeID) override;
    void unbanNode(NodeID nodeID) override;
    bool isBanned(NodeID nodeID) override;
    std::vector<std::string> getBans() override;
};
}
