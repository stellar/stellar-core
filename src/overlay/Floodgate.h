#ifndef __FLOODGATE__
#define __FLOODGATE__


#include "generated/stellar.hh"
#include "overlay/Peer.h"
#include "overlay/StellarMessage.h"

/*
Keeps track of what peers have sent us which flood messages so we know who to send to when we broadcast the messages in return

Transactions  (txID) 
Prepare		(sig I know they could be malleable but that doesn't matter in this case the worse thing would be we would flood twice)
Aborted
Commit
Committed

*/

namespace stellar
{
    class PeerMaster;

	class FloodRecord
	{
	public:
		typedef std::shared_ptr<FloodRecord> pointer;

		uint32_t mLedgerIndex;
        StellarMessagePtr mMessage;
		vector<Peer::pointer> mPeersTold;

		FloodRecord(StellarMessagePtr msg, uint32_t ledger, Peer::pointer peer);

	};

	class Floodgate
	{
		map< stellarxdr::uint256, FloodRecord::pointer > mFloodMap; 
	public:

		// Floodgate will be cleared after every ledger close
		void clearBelow(uint32_t currentLedger);
		void addRecord(stellarxdr::uint256 index, StellarMessagePtr msg, uint32_t ledgerIndex, Peer::pointer peer);

		void broadcast(stellarxdr::uint256 index,PeerMaster* peerMaster);

	};
}

#endif