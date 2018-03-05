#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transport/PeerRecord.h"
#include "transport/TCPAcceptor.h"

namespace medida
{
class Meter;
class Counter;
}

/*
Maintain the set of peers we are connected to
*/
namespace stellar
{

class Transport
{
  protected:
    //     friend class OverlayManagerImpl;
    friend class OverlayManagerTests;

    Application& mApp;

    // pending peers - connected, but not authenticated
    std::vector<Peer::pointer> mPendingPeers;
    // authenticated and connected peers
    std::map<NodeID, Peer::pointer> mAuthenticatedPeers;
    bool mShuttingDown;

    medida::Meter& mConnectionsAttempted;
    medida::Meter& mConnectionsEstablished;
    medida::Meter& mConnectionsDropped;
    medida::Meter& mConnectionsRejected;
    medida::Counter& mPendingPeersSize;
    medida::Counter& mAuthenticatedPeersSize;

  public:
    Transport(Application& app);
    ~Transport();

    void connectTo(std::string const& addr);
    virtual void connectTo(PeerRecord& pr);
    void connectTo(PeerBareAddress const& address);

    void addPendingPeer(Peer::pointer peer);
    void dropPeer(Peer* peer);
    std::vector<Peer::pointer> const& getPendingPeers() const;
    int getPendingPeersCount() const;
    std::map<NodeID, Peer::pointer> const& getAuthenticatedPeers() const;
    int getAuthenticatedPeersCount() const;
    bool acceptAuthenticatedPeer(Peer::pointer peer);
    bool moveToAuthenticated(Peer::pointer peer);

    // returns nullptr if the passed peer isn't found
    Peer::pointer getConnectedPeer(PeerBareAddress const& address);

    std::vector<Peer::pointer> getRandomAuthenticatedPeers();

    void shutdown();

    bool isShuttingDown() const;

  private:
    void updateSizeCounters();
    void noteHandshakeSuccessInPeerRecord(Peer::pointer peer);
};
}
