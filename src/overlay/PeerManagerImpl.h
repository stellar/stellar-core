#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/PeerManager.h"
#include "util/Timer.h"

#include <functional>

namespace soci
{
class statement;
}

namespace stellar
{

class Database;
class RandomPeerSource;

class PeerManagerImpl : public PeerManager
{
  public:
    explicit PeerManagerImpl(Application& app);

    void ensureExists(PeerBareAddress const& address) override;
    void update(PeerBareAddress const& address, TypeUpdate type) override;
    void update(PeerBareAddress const& address, BackOffUpdate backOff) override;
    void update(PeerBareAddress const& address, TypeUpdate type,
                BackOffUpdate backOff) override;
    std::pair<PeerRecord, bool> load(PeerBareAddress const& address) override;
    void store(PeerBareAddress const& address, PeerRecord const& PeerRecord,
               bool inDatabase) override;
    std::vector<PeerBareAddress> loadRandomPeers(PeerQuery const& query,
                                                 int size) override;
    void removePeersWithManyFailures(
        int minNumFailures, PeerBareAddress const* address = nullptr) override;
    std::vector<PeerBareAddress>
    getPeersToSend(int size, PeerBareAddress const& address) override;

  protected:
    Application& mApp;

  private:
    std::unique_ptr<RandomPeerSource> mOutboundPeersToSend;
    std::unique_ptr<RandomPeerSource> mInboundPeersToSend;

    int countPeers(std::string const& where,
                   std::function<void(soci::statement&)> const& bind);
    std::vector<PeerBareAddress>
    loadPeers(int limit, int offset, std::string const& where,
              std::function<void(soci::statement&)> const& bind);

    void update(PeerRecord& peer, TypeUpdate type);
    void update(PeerRecord& peer, BackOffUpdate backOff, Application& app);
};
}
