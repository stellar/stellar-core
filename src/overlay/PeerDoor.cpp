#include <memory>
#include "PeerDoor.h"
#include "main/Application.h"
#include "Peer.h"
#include "overlay/PeerMaster.h"
#include "lib/util/Logging.h"

namespace stellar
{
    PeerDoor::PeerDoor(Application &app)
        : mApp(app)
        , mAcceptor(mApp.getMainIOService())
    {
        if(!mApp.mConfig.RUN_STANDALONE) {
            asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), mApp.mConfig.PEER_PORT);
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
        auto sock = std::make_shared<asio::ip::tcp::socket>(mApp.getMainIOService());
        mAcceptor.async_accept(*sock, [this, sock](asio::error_code const &ec) {
                if (ec)
                    this->acceptNextPeer();
                else
                    this->handleKnock(sock);
            });
    }

    void PeerDoor::handleKnock(shared_ptr<asio::ip::tcp::socket> socket)
    {
        LOG(DEBUG) << "PeerDoor handleKnock()";
        Peer::pointer peer = std::make_shared<Peer>(mApp, socket);
        mApp.getPeerMaster().addPeer(peer);
        peer->createFromDoor();
        acceptNextPeer();
    }
}

