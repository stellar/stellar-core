#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transport/Peer.h"
#include "transport/PeerRecord.h"

#include <set>
#include <string>

namespace stellar
{

class Application;

class PreferredPeers
{
  public:
    explicit PreferredPeers(Application& app);
    ~PreferredPeers();

    std::set<std::string> const& getAll() const;
    bool isPreferred(Peer* peer);
    void orderByPreferredPeers(std::vector<PeerRecord>& peers);
    void storeConfigPeers();
    void storePeerList(std::vector<std::string> const& list, bool resetBackOff,
                       bool preferred);

  private:
    Application& mApp;
    std::set<std::string> mPreferredPeers;
};
}
