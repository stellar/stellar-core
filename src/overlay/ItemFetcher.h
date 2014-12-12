#ifndef __ITEMFETCHER__
#define __ITEMFETCHER__

#include <map>
#include "txherder/TransactionSet.h"
#include "overlay/Peer.h"
#include "fba/QuorumSet.h"
#include "clf/CLF.h"
#include "util/timer.h"

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
        Application &mApp;
        Peer::pointer mLastAskedPeer;
		vector<Peer::pointer> mPeersAsked;
		bool mCantFind;
        Timer mTimer;
        int mRefCount;

	protected:
        
		virtual void askPeer(Peer::pointer peer) = 0;
	public:
		typedef std::shared_ptr<TrackingCollar> pointer;

        stellarxdr::uint256 mItemID;

		virtual bool isItemFound() = 0;

		TrackingCollar(stellarxdr::uint256 const& id, Application &app);

        void doesntHave(Peer::pointer peer);
		void tryNextPeer();
        void cancelFetch();
        void refInc() { mRefCount++; }
        void refDec();
        int getRefCount() { return mRefCount;  }
	};

	

	class ItemFetcher
	{
	protected:
        Application &mApp;
        std::map<stellarxdr::uint256, TrackingCollar::pointer> mItemMap;
	public:
        ItemFetcher(Application &app) : mApp(app) {}
		void clear();
        void stopFetching(stellarxdr::uint256 const& itemID);
        void stopFetchingAll();
        void doesntHave(stellarxdr::uint256 const& itemID, Peer::pointer peer);
	};

    // We want to keep the last N ledgers worth of Txsets around 
    //    in case there are stragglers still trying to close
	class TxSetFetcher : public ItemFetcher
	{
	public:
        TxSetFetcher(Application &app) : ItemFetcher(app) {}
		TransactionSet::pointer fetchItem(stellarxdr::uint256 const& itemID, bool askNetwork);
		// looks to see if we know about it but doesn't ask the network
		TransactionSet::pointer findItem(stellarxdr::uint256 const& itemID);
		bool recvItem(TransactionSet::pointer txSet);

	};


	class QSetFetcher : public ItemFetcher
	{
	public:
        
        QSetFetcher(Application &app) : ItemFetcher(app) {}
		QuorumSet::pointer fetchItem(stellarxdr::uint256 const& itemID, bool askNetwork);
		// looks to see if we know about it but doesn't ask the network
        QuorumSet::pointer findItem(stellarxdr::uint256 const& itemID);
		void recvItem(QuorumSet::pointer qSet);
	};

    class DeltaFetcher : public ItemFetcher
    {
    public:
        DeltaFetcher(Application &app) : ItemFetcher(app) {}
        void fetchItem(stellarxdr::uint256 const& itemID, uint32_t oldLedgerSeq);
        void recvItem(CLFDeltaPtr delta);
    };

    class DeltaTrackingCollar : public TrackingCollar
    {
        void askPeer(Peer::pointer peer);
    public:

    };

	class TxSetTrackingCollar : public TrackingCollar
	{

		void askPeer(Peer::pointer peer);
	public:
		TransactionSet::pointer mTxSet;

        TxSetTrackingCollar(stellarxdr::uint256 const& id,  Application &app);
		bool isItemFound(){ return(!!mTxSet); }
	};

	class QSetTrackingCollar : public TrackingCollar
	{
		void askPeer(Peer::pointer peer);
	public:
		QuorumSet::pointer mQSet;

        QSetTrackingCollar(stellarxdr::uint256 const& id, Application &app);
		bool isItemFound(){ return(!!mQSet); }
	};
	
}

#endif
