#ifndef __ITEMFETCHER__
#define __ITEMFETCHER__

#include "txherder/TransactionSet.h"
#include "overlay/Peer.h"
#include "fba/QuorumSet.h"

/*
Manages asking for Transaction or Quorum sets from Peers

LATER: This abstraction can be cleaned up it is a bit wonky

Asks for TransactionSets from our Peers
We need to get these for FBA.
Anywhere else? If someone asked you you can late reply to them

*/

namespace stellar
{
	class TrackingCollar
	{
        Peer::pointer mLastAskedPeer;
		vector<Peer::pointer> mPeersAsked;
		bool mCantFind;
        boost::asio::deadline_timer mTimer;
        int mRefCount;

	protected:
        
		virtual void askPeer(Peer::pointer peer) = 0;
	public:
		typedef std::shared_ptr<TrackingCollar> pointer;

		
        stellarxdr::uint256 mItemID;

		virtual bool isItemFound() = 0;

		TrackingCollar(stellarxdr::uint256& id, ApplicationPtr app);

        void doesntHave(Peer::pointer peer,ApplicationPtr app);
		void tryNextPeer(ApplicationPtr app);
        void cancelFetch();
        void refInc() { mRefCount++; }
        void refDec();
        int getRefCount() { return mRefCount;  }
	};

	

	class ItemFetcher
	{
	protected:
        ApplicationPtr mApp;
		map<stellarxdr::uint256, TrackingCollar::pointer> mItemMap;
	public:
		void clear();
        void stopFetching(stellarxdr::uint256& itemID);
        void stopFetchingAll();
        void doesntHave(stellarxdr::uint256& itemID, Peer::pointer peer, ApplicationPtr app);
	};

    // We want to keep the last N ledgers worth of Txsets around 
    //    in case there are stragglers still trying to close
	class TxSetFetcher : public ItemFetcher
	{
	public:
		TransactionSet::pointer fetchItem(stellarxdr::uint256& itemID, bool askNetwork);
		// looks to see if we know about it but doesn't ask the network
		TransactionSet::pointer findItem(stellarxdr::uint256& itemID);
		bool recvItem(TransactionSet::pointer txSet);

	};


	class QSetFetcher : public ItemFetcher
	{
	public:
        
		QuorumSet::pointer fetchItem(stellarxdr::uint256& itemID, bool askNetwork);
		// looks to see if we know about it but doesn't ask the network
        QuorumSet::pointer findItem(stellarxdr::uint256& itemID);
		void recvItem(ApplicationPtr app,QuorumSet::pointer qSet);
	};

	class TxSetTrackingCollar : public TrackingCollar
	{

		void askPeer(Peer::pointer peer);
	public:
		TransactionSet::pointer mTxSet;

        TxSetTrackingCollar(stellarxdr::uint256& id,  ApplicationPtr app);
		bool isItemFound(){ return(!!mTxSet); }
	};

	class QSetTrackingCollar : public TrackingCollar
	{
		void askPeer(Peer::pointer peer);
	public:
		QuorumSet::pointer mQSet;

        QSetTrackingCollar(stellarxdr::uint256& id, ApplicationPtr app);
		bool isItemFound(){ return(!!mQSet); }
	};
	
}

#endif