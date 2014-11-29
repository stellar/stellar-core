#ifndef __PEERMASTER__
#define __PEERMASTER__

#include "Peer.h"
#include "PeerDoor.h"
#include "overlay/ItemFetcher.h"
#include "overlay/Floodgate.h"
#include <vector>
#include <thread>
#include "generated/stellar.hh"
#include "overlay/OverlayGateway.h"

using namespace std;
/*
Maintain the set of peers we are connected to
*/
namespace stellar
{

	class PeerMaster : public OverlayGateway
	{
		std::thread mPeerThread;

		vector<Peer::pointer> mPeers;
		PeerDoor mDoor;
		QSetFetcher mQSetFetcher;
        ApplicationPtr mApp;

		void addConfigPeers();

		void run();
	public:
		boost::asio::io_service mIOservice;
		Floodgate mFloodGate;

		PeerMaster();
		~PeerMaster();

        void setApplication(ApplicationPtr app) { mApp = app; }
		//////// GATEWAY FUNCTIONS
		void ledgerClosed(LedgerPtr ledger);

		QuorumSet::pointer fetchQuorumSet(stellarxdr::uint256& itemID){ return(mQSetFetcher.fetchItem(itemID)); }
        void recvFloodedMsg(stellarxdr::uint256 index, StellarMessagePtr msg, uint32_t ledgerIndex, Peer::pointer peer) { mFloodGate.addRecord(index, msg, ledgerIndex, peer);  }
        void doesntHaveQSet(stellarxdr::uint256 index, Peer::pointer peer) { mQSetFetcher.doesntHave(index, peer,mApp); }

        void broadcastMessage(StellarMessagePtr msg, Peer::pointer peer);
        void recvQuorumSet(QuorumSet::pointer qset);
		//////


		void start();
		void addPeer(Peer::pointer peer);
		void dropPeer(Peer::pointer peer);

		Peer::pointer getRandomPeer();
		Peer::pointer getNextPeer(Peer::pointer peer); // returns NULL if the passed peer isn't found

		
		void broadcastMessage(stellarxdr::uint256& msgID);
		void broadcastMessage(StellarMessagePtr msg, vector<Peer::pointer>& skip);
	};
}

#endif