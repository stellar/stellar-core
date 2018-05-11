#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transport/MessageHandler.h"

namespace stellar
{
class OverlayMessageHandler : public MessageHandler
{
  public:
    explicit OverlayMessageHandler(Application& app);
    ~OverlayMessageHandler() = default;

    bool shouldAbort(Peer::const_pointer peer) override;
    std::pair<bool, std::string> acceptHello(Peer::pointer peer,
                                             Hello const& elo) override;
    bool acceptAuthenticated(Peer::pointer peer) override;
    void getSCPState(Peer::pointer, uint32 seq) override;
    void getPeers(Peer::pointer peer) override;
    void peers(Peer::pointer peer,
               std::vector<PeerAddress> const& peers) override;

    void doesNotHave(Peer::pointer peer, ItemKey itemKey) override;
    void getTxSet(Peer::pointer peer, Hash const& hash) override;
    void getQuorumSet(Peer::pointer peer, Hash const& hash) override;
    std::set<SCPEnvelope> txSet(Peer::pointer peer, TransactionSet const& txSet,
                                bool force = false) override;
    std::set<SCPEnvelope> quorumSet(Peer::pointer peer,
                                    SCPQuorumSet const& qSet) override;

  private:
    Application& mApp;

    medida::Meter& mDropInRecvHelloSelfMeter;
    medida::Meter& mDropInRecvHelloPeerIDMeter;
    medida::Meter& mDropInRecvHelloCertMeter;
    medida::Meter& mDropInRecvHelloBanMeter;
    medida::Meter& mDropInRecvHelloNetMeter;
};
}
