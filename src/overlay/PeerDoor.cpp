#include "PeerDoor.h"
#include "main/Application.h"
#include "Peer.h"
#include "overlay/PeerMaster.h"

namespace stellar
{
	PeerDoor::PeerDoor() 
	{
        mAcceptor = NULL;
	}
	
	void PeerDoor::start()
	{
		mAcceptor = new boost::asio::ip::tcp::acceptor(gPeerMaster.mIOservice);
		boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::tcp::v4(), gApp.mConfig.PEER_PORT);
		mAcceptor->open(endpoint.protocol());
		mAcceptor->bind(endpoint);
		mAcceptor->listen();

		acceptNextPeer();
	}

	void PeerDoor::close()
	{
		mAcceptor->cancel();
	}

	void PeerDoor::acceptNextPeer()
	{
		shared_ptr<boost::asio::ip::tcp::socket> pSocket(new boost::asio::ip::tcp::socket(gPeerMaster.mIOservice));

		mAcceptor->async_accept(*pSocket, bind(&PeerDoor::handleKnock, this, pSocket));
	}

	void PeerDoor::handleKnock(shared_ptr<boost::asio::ip::tcp::socket> socket)
	{
		Peer::pointer peer(new Peer(socket));
		gPeerMaster.addPeer(peer);
		peer->createFromDoor();

		acceptNextPeer();
	}
}

