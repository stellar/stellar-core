#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/util/lrucache.hpp"
#include "xdr/Stellar-types.h"

#include "herder/Herder.h"
#include "xdr/Stellar-types.h"

#include <map>
#include <medida/medida.h>
#include <set>

namespace stellar
{

class Application;

class NodesInQuorum
{
  public:
    explicit NodesInQuorum(Application& app);

    // returns true if we think that the node is in quorum
    bool isNodeInQuorum(NodeID const& node);
    void clear();

  private:
    Application& mApp;
    cache::lru_cache<NodeID, bool> mNodesInQuorum;
};
}
