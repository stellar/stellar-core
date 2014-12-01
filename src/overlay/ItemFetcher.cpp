#include "ItemFetcher.h"
#include "main/Application.h"

#define MS_TO_WAIT_FOR_FETCH_REPLY 3000

/*

*/

namespace stellar
{
    void ItemFetcher::doesntHave(stellarxdr::uint256& itemID, Peer::pointer peer,Application::pointer app)
    {
        auto result = mItemMap.find(itemID);
        if(result != mItemMap.end())
        { // found
            result->second->doesntHave(peer,app);
        }
    }
    void ItemFetcher::stopFetchingAll()
    {
        for(auto result : mItemMap)
        {
            result.second->cancelFetch();
        }
    }

    //LATER  Do we ever need to call this
    void ItemFetcher::stopFetching(stellarxdr::uint256& itemID)
    {
        auto result = mItemMap.find(itemID);
        if(result != mItemMap.end())
        {
            result->second->refDec();
        }
    }

    void ItemFetcher::clear()
    {
        mItemMap.clear();
    }
   

	//////////////////////////////
   

	TransactionSet::pointer TxSetFetcher::fetchItem(stellarxdr::uint256& setID,bool askNetwork)
	{
		// look it up in the map
		// if not found then start fetching
		auto result = mItemMap.find(setID);
		if(result != mItemMap.end())
		{ // collar found
			if(result->second->isItemFound())
			{
				return ((TxSetTrackingCollar*)result->second.get())->mTxSet;
			} else
			{
				result->second->refInc();
			}
			
		} else
		{  // not found 
            if(askNetwork)
            {
                TrackingCollar::pointer collar = TrackingCollar::pointer(new TxSetTrackingCollar(setID,mApp));
                mItemMap[setID] = collar;
                collar->tryNextPeer(mApp);
            }
		}
		return (TransactionSet::pointer());
	}

	// returns true if we were waiting for this txSet
	bool TxSetFetcher::recvItem(TransactionSet::pointer txSet)
	{
		if(txSet)
		{
			auto result = mItemMap.find(txSet->getContentsHash());
			if(result != mItemMap.end())
			{
                result->second->cancelFetch();
				((TxSetTrackingCollar*)result->second.get())->mTxSet = txSet;
				if(result->second->getRefCount())
				{  // someone was still interested in this tx set so tell FBA  LATER: maybe change this to pub/sub
					return true;
				}
			} else
			{  // doesn't seem like we were looking for it. Maybe just add it for now 
				mItemMap[txSet->getContentsHash()] = TrackingCollar::pointer(new TxSetTrackingCollar(txSet->getContentsHash(),mApp));
			}
		}
		return false;
	}

	////////////////////////////////////////
	void QSetFetcher::recvItem(Application::pointer app, QuorumSet::pointer qSet)
	{
		if(qSet)
		{
			auto result = mItemMap.find(qSet->getHash());
			if(result != mItemMap.end())
			{
                result->second->cancelFetch();
				((QSetTrackingCollar*)result->second.get())->mQSet = qSet;
				if(result->second->getRefCount())
				{  // someone was still interested in this quorum set so tell FBA  LATER: maybe change this to pub/sub
					app->getFBAGateway().addQuorumSet(qSet);
				}
			} else
			{  // doesn't seem like we were looking for it. Maybe just add it for now 
				mItemMap[qSet->getHash()] = TrackingCollar::pointer(new QSetTrackingCollar(qSet->getHash(),mApp));
			}
		}
	}

	QuorumSet::pointer QSetFetcher::fetchItem(stellarxdr::uint256& setID, bool askNetwork)
	{
		// look it up in the map
		// if not found then start fetching
		auto result = mItemMap.find(setID);
		if(result != mItemMap.end())
		{ // collar found
			if(result->second->isItemFound())
			{
				return ((QSetTrackingCollar*)result->second.get())->mQSet;
			} else
			{
				result->second->refInc();
			}

		} else
		{  // not found 
            if(askNetwork)
            {
                TrackingCollar::pointer collar=TrackingCollar::pointer(new QSetTrackingCollar(setID,mApp));
                mItemMap[setID] = collar;
                collar->tryNextPeer(mApp); // start asking
            }
		}
		return (QuorumSet::pointer());
	}

    //////////////////////////////////////////////////////////////////////////

	TrackingCollar::TrackingCollar(stellarxdr::uint256& id, ApplicationPtr app) : 
        mItemID(id), mTimer(*(app->getPeerMaster().mIOservice))
	{
		mCantFind = false;
		mRefCount = 1;
        
	}

   

    void TrackingCollar::doesntHave(Peer::pointer peer,Application::pointer app)
    {
        if(mLastAskedPeer == peer)
        {
            tryNextPeer(app);
        }
    }

    void TrackingCollar::refDec()
    {
        mRefCount--;
        if(mRefCount < 1) cancelFetch();
    }

    void TrackingCollar::cancelFetch()
    {
        mTimer.cancel();
    }
	

	// will be called by some timer or when we get a result saying they don't have it
	void TrackingCollar::tryNextPeer(Application::pointer app)
	{
		if(!isItemFound())
		{	// we still haven't found this item
			Peer::pointer peer;

			if(mPeersAsked.size())
			{
				while(!peer && mPeersAsked.size())
				{  // keep looping till we find a peer we are still connected to
					peer = app->getOverlayGateway().getNextPeer(mPeersAsked[mPeersAsked.size() - 1]);
					if(!peer) mPeersAsked.pop_back();
				}
			} else
			{
				peer = app->getOverlayGateway().getRandomPeer();
			}

			if(peer)
			{
				if(find(mPeersAsked.begin(), mPeersAsked.end(), peer) == mPeersAsked.end())
				{ // we have never asked this guy
                    mLastAskedPeer = peer;
                    
                    mTimer.cancel(); // cancel any stray timers
                    auto fun = std::bind(&TrackingCollar::tryNextPeer, this, app);
                    mTimer.expires_from_now(boost::posix_time::milliseconds(MS_TO_WAIT_FOR_FETCH_REPLY));
                    mTimer.async_wait(fun);
                    
                    
					askPeer(peer); 
					mPeersAsked.push_back(peer);
				} else
				{  // we have looped back around 
					mCantFind = true;
					// LATER what should we do here?
                    // try to connect to more peers?
                    // just ask any new peers we connect to?
                    // wait a longer amount of time and then loop again?
				}

			}
		}
	}

    QSetTrackingCollar::QSetTrackingCollar(stellarxdr::uint256& id, ApplicationPtr app) : TrackingCollar(id,app)
    {

    }

	void QSetTrackingCollar::askPeer(Peer::pointer peer)
	{
		peer->sendGetQuorumSet(mItemID);
	}

    TxSetTrackingCollar::TxSetTrackingCollar(stellarxdr::uint256& id,  ApplicationPtr app) : TrackingCollar(id,app)
    {
  
    }

	void TxSetTrackingCollar::askPeer(Peer::pointer peer)
	{
		peer->sendGetTxSet(mItemID);
	}
}
