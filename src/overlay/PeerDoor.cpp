#include <memory>
#include "PeerDoor.h"
#include "main/Application.h"
#include "Peer.h"
#include "overlay/PeerMaster.h"
#include "lib/util/Logging.h"

namespace stellar
{

    using asio::ip::tcp;
    using std::make_shared;

    PeerDoor::PeerDoor(Application &app)
        : mApp(app)
        , mAcceptor(mApp.getMainIOService())
    {
        if(!mApp.mConfig.RUN_STANDALONE) {
            tcp::endpoint endpoint(tcp::v4(), mApp.mConfig.PEER_PORT);
            LOG(DEBUG) << "PeerDoor binding to endpoint " << endpoint;
            mAcceptor.open(endpoint.protocol());
            mAcceptor.bind(endpoint);
            mAcceptor.listen();
            acceptNextPeer();
        }
    }

    void PeerDoor::close()
    {
        mAcceptor.cancel();
    }

    void PeerDoor::acceptNextPeer()
    {
        LOG(DEBUG) << "PeerDoor acceptNextPeer()";
        auto sock = make_shared<tcp::socket>(mApp.getMainIOService());
        mAcceptor.async_accept(*sock, [this, sock](asio::error_code const &ec) {
                if (ec)
                    this->acceptNextPeer();
                else
                    this->handleKnock(sock);
            });
    }

    void PeerDoor::handleKnock(shared_ptr<tcp::socket> socket)
    {
        LOG(DEBUG) << "PeerDoor handleKnock()";
        Peer::pointer peer = make_shared<TCPPeer>(mApp, socket, Peer::ACCEPTOR);
        mApp.getPeerMaster().addPeer(peer);
        acceptNextPeer();
    }
}

