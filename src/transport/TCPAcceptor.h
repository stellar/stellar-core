#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "TCPPeer.h"

/*
listens for peer connections.
When found passes them to the OverlayManagerImpl
*/

namespace stellar
{
class Application;

class TCPAcceptor
{
  public:
    using newPeerCallback = std::function<void(Peer::pointer)>;

    explicit TCPAcceptor(Application& app, newPeerCallback peerCallback);

    void start();
    void close();

  private:
    Application& mApp;
    newPeerCallback mPeerCallback;
    asio::ip::tcp::acceptor mAcceptor;
    bool mShuttingDown{false};

    virtual void acceptNextPeer();
    virtual void handleKnock(std::shared_ptr<TCPPeer::SocketType> pSocket);
};
}
