// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "PeerDoor.h"
#include "Peer.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/OverlayManager.h"
#include "overlay/TCPPeer.h"
#include "util/Logging.h"
#include <memory>

namespace stellar
{
constexpr uint32 const LISTEN_QUEUE_LIMIT = 100;

using asio::ip::tcp;
using namespace std;

PeerDoor::PeerDoor(Application& app)
    : mApp(app), mAcceptor(mApp.getClock().getIOContext())
{
}

void
PeerDoor::start()
{
    releaseAssert(threadIsMain());

    if (!mApp.getConfig().RUN_STANDALONE)
    {
        tcp::endpoint endpoint(tcp::v4(), mApp.getConfig().PEER_PORT);
        CLOG_INFO(Overlay, "Binding to endpoint {}:{}",
                  endpoint.address().to_string(), endpoint.port());
        mAcceptor.open(endpoint.protocol());
        mAcceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
        mAcceptor.bind(endpoint);
        mAcceptor.listen(LISTEN_QUEUE_LIMIT);
        acceptNextPeer();
    }
}

void
PeerDoor::close()
{
    if (mAcceptor.is_open())
    {
        asio::error_code ec;
        // ignore errors when closing
        std::ignore = mAcceptor.close(ec);
    }
}

void
PeerDoor::acceptNextPeer()
{
    if (mApp.getOverlayManager().isShuttingDown())
    {
        return;
    }

    CLOG_DEBUG(Overlay, "PeerDoor acceptNextPeer()");
    // Asio guarantees it is safe to create a socket object with overlay's
    // io_context on main (as long as the socket is not accessed by multiple
    // threads simultaneously, or the caller manually synchronizes access to the
    // socket).
    auto& ioContext = mApp.getConfig().BACKGROUND_OVERLAY_PROCESSING
                          ? mApp.getOverlayIOContext()
                          : mApp.getClock().getIOContext();
    auto sock = make_shared<TCPPeer::SocketType>(ioContext, TCPPeer::BUFSZ);
    mAcceptor.async_accept(sock->next_layer(),
                           [this, sock](asio::error_code const& ec) {
                               releaseAssert(threadIsMain());
                               if (ec)
                                   this->acceptNextPeer();
                               else
                                   this->handleKnock(sock);
                           });
}

void
PeerDoor::handleKnock(shared_ptr<TCPPeer::SocketType> socket)
{
    releaseAssert(threadIsMain());

    CLOG_DEBUG(Overlay, "PeerDoor handleKnock()");
    Peer::pointer peer = TCPPeer::accept(mApp, socket);

    // Still call addInboundConnection to update metrics
    mApp.getOverlayManager().maybeAddInboundConnection(peer);

    if (!peer)
    {
        asio::error_code ec;
        std::ignore = socket->close(ec);
        if (ec)
        {
            CLOG_WARNING(Overlay, "TCPPeer: close socket failed: {}",
                         ec.message());
        }
    }
    acceptNextPeer();
}
}
