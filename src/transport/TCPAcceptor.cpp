// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "TCPAcceptor.h"
#include "main/Application.h"
#include "util/Logging.h"

namespace stellar
{

using asio::ip::tcp;
using namespace std;

TCPAcceptor::TCPAcceptor(Application& app, newPeerCallback peerCallback)
    : mApp{app}
    , mPeerCallback{peerCallback}
    , mAcceptor{mApp.getClock().getIOService()}
{
    assert(mPeerCallback);
}

void
TCPAcceptor::start()
{
    tcp::endpoint endpoint(tcp::v4(), mApp.getConfig().PEER_PORT);
    CLOG(DEBUG, "Overlay") << "TCPAcceptor binding to endpoint " << endpoint;
    mAcceptor.open(endpoint.protocol());
    mAcceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    mAcceptor.bind(endpoint);
    mAcceptor.listen();
    acceptNextPeer();
}

void
TCPAcceptor::close()
{
    if (mShuttingDown)
    {
        return;
    }

    mShuttingDown = true;
    if (mAcceptor.is_open())
    {
        asio::error_code ec;
        // ignore errors when closing
        mAcceptor.close(ec);
    }
}

void
TCPAcceptor::acceptNextPeer()
{
    if (mShuttingDown)
    {
        return;
    }

    CLOG(DEBUG, "Overlay") << "TCPAcceptor acceptNextPeer()";
    auto sock =
        make_shared<TCPPeer::SocketType>(mApp.getClock().getIOService());
    mAcceptor.async_accept(sock->next_layer(),
                           [this, sock](asio::error_code const& ec) {
                               if (ec)
                                   this->acceptNextPeer();
                               else
                                   this->handleKnock(sock);
                           });
}

void
TCPAcceptor::handleKnock(shared_ptr<TCPPeer::SocketType> socket)
{
    if (mShuttingDown)
    {
        return;
    }

    CLOG(DEBUG, "Overlay") << "TCPAcceptor handleKnock() @"
                           << mApp.getConfig().PEER_PORT;
    auto peer = TCPPeer::accept(mApp, socket);
    if (peer)
    {
        mPeerCallback(peer);
    }
    acceptNextPeer();
}
}
