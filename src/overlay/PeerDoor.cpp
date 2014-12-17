// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <memory>
#include "PeerDoor.h"
#include "main/Application.h"
#include "Peer.h"
#include "overlay/PeerMaster.h"
#include "util/Logging.h"
#include "overlay/TCPPeer.h"

namespace stellar
{

using asio::ip::tcp;
using std::make_shared;

PeerDoor::PeerDoor(Application& app)
    : mApp(app), mAcceptor(mApp.getMainIOService())
{
    if (!mApp.mConfig.RUN_STANDALONE)
    {
        tcp::endpoint endpoint(tcp::v4(), mApp.mConfig.PEER_PORT);
        CLOG(DEBUG, "Overlay") << "PeerDoor binding to endpoint " << endpoint;
        mAcceptor.open(endpoint.protocol());
        mAcceptor.bind(endpoint);
        mAcceptor.listen();
        acceptNextPeer();
    }
}

void
PeerDoor::close()
{
    mAcceptor.cancel();
}

void
PeerDoor::acceptNextPeer()
{
    CLOG(DEBUG, "Overlay") << "PeerDoor acceptNextPeer()";
    auto sock = make_shared<tcp::socket>(mApp.getMainIOService());
    mAcceptor.async_accept(*sock, [this, sock](asio::error_code const& ec)
                           {
        if (ec)
            this->acceptNextPeer();
        else
            this->handleKnock(sock);
    });
}

void
PeerDoor::handleKnock(shared_ptr<tcp::socket> socket)
{
    CLOG(DEBUG, "Overlay") << "PeerDoor handleKnock()";
    Peer::pointer peer = make_shared<TCPPeer>(mApp, socket, Peer::ACCEPTOR);
    mApp.getPeerMaster().addPeer(peer);
    acceptNextPeer();
}
}
