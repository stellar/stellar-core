#ifndef __OVERLAYGATEWAY__
#define __OVERLAYGATEWAY__

#include "overlay/StellarMessage.h"
#include "generated/stellar.hh"

/*
Public interface to the Overlay module

Maintains the connection to the Stellar network
Handles flooding the appropriate messages to the rest of the world
There are 3 classes of messages:
-Messages to a particular Peer that want a response:
    Hello,  getPeers
-Messages that are flooded to the network:
    transaction, prepare, prepared, etc...
-Messages that you want a response from some peer but you don't care which. Should keep rotating peers till you get an answer:
    getTxSet, getQuorumSet, getHistory, getDelta, ...


*/

namespace stellar
{
	class Ledger;
	typedef std::shared_ptr<Ledger> LedgerPtr;

	class OverlayGateway
	{
	public:


		//called by Ledger
		virtual void ledgerClosed(LedgerPtr ledger) = 0;
        virtual void fetchDelta(stellarxdr::uint256& oldLedgerHash, uint32_t oldLedgerSeq) = 0;

		// called by TxHerder
		virtual void broadcastMessage(stellarxdr::uint256& messageID) = 0;

		//called by FBA
		virtual QuorumSet::pointer fetchQuorumSet(stellarxdr::uint256& itemID,bool askNetwork)=0;

		// called internally and by FBA
		virtual void broadcastMessage(StellarMessagePtr msg, Peer::pointer peer)=0;
        //virtual Item trackDownItem(stellarxdr::uint256 itemID, stellarxdr::StellarMessage& msg) = 0;  needs to return Item this is the issue
        //virtual void stopTrackingItem(stellarxdr::uint256 itemID) = 0;


        // called internally
        virtual void recvQuorumSet(QuorumSet::pointer qset) = 0;
        virtual void doesntHaveQSet(stellarxdr::uint256 index, Peer::pointer peer) = 0;
        virtual void recvFloodedMsg(stellarxdr::uint256 index, StellarMessagePtr msg, uint32_t ledgerIndex, Peer::pointer peer) = 0;
        virtual Peer::pointer getRandomPeer()=0;
        virtual Peer::pointer getNextPeer(Peer::pointer peer)=0; // returns NULL if the passed peer isn't found
	};
}

#endif