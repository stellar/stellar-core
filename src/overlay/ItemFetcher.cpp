#include "ItemFetcher.h"
#include "main/Application.h"

/*
SANITY: we need some sort of timer callback to move this along
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

	void ItemFetcher::stopFetching(stellarxdr::uint256& itemID)
	{
		auto result = mItemMap.find(itemID);
		if(result != mItemMap.end())
		{ // found
			if(result->second->mRefCount)
				result->second->mRefCount--;
		} 
	}

	void ItemFetcher::clear()
	{
		mItemMap.clear();
	}

	//////////////////////////////
	TransactionSet::pointer TxSetFetcher::fetchItem(stellarxdr::uint256& setID)
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
				result->second->mRefCount++;
			}
			
		} else
		{  // not found 
			mItemMap[setID] = TrackingCollar::pointer(new TxSetTrackingCollar(setID));
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
				((TxSetTrackingCollar*)result->second.get())->mTxSet = txSet;
				if(result->second->mRefCount)
				{  // someone was still interested in this tx set so tell Firmeza  LATER: maybe change this to pub/sub
					return true;
				}
			} else
			{  // doesn't seem like we were looking for it. Maybe just add it for now 
				mItemMap[txSet->getContentsHash()] = TrackingCollar::pointer(new TxSetTrackingCollar(txSet->getContentsHash()));
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
				((QSetTrackingCollar*)result->second.get())->mQSet = qSet;
				if(result->second->mRefCount)
				{  // someone was still interested in this tx set so tell Firmeza  LATER: maybe change this to pub/sub
					app->getFBAGateway().addQuorumSet(qSet);
				}
			} else
			{  // doesn't seem like we were looking for it. Maybe just add it for now 
				mItemMap[qSet->getHash()] = TrackingCollar::pointer(new QSetTrackingCollar(qSet->getHash()));
			}
		}
	}

	QuorumSet::pointer QSetFetcher::fetchItem(stellarxdr::uint256& setID)
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
				result->second->mRefCount++;
			}

		} else
		{  // not found 
			mItemMap[setID] = TrackingCollar::pointer(new QSetTrackingCollar(setID));
		}
		return (QuorumSet::pointer());
	}

    //////////////////////////////////////////////////////////////////////////

	TrackingCollar::TrackingCollar(stellarxdr::uint256& id) : mItemID(id)
	{
		mCantFind = false;
		mRefCount = 1;
        mTimeAsked = std::chrono::system_clock::now();  // TODO: better time here?
	}

    void TrackingCollar::doesntHave(Peer::pointer peer,Application::pointer app)
    {
        if(mLastAskedPeer == peer)
        {
            tryNextPeer(app);
        }
    }
	

	// SANITY: will be called by some timer or when we get a result saying they don't have it
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
                    mTimeAsked= std::chrono::system_clock::now();
					askPeer(peer); 
					mPeersAsked.push_back(peer);
				} else
				{  // we have looped back around 
					mCantFind = true;
					// SANITY what should we do here?
				}

			}
		}
	}

    QSetTrackingCollar::QSetTrackingCollar(stellarxdr::uint256& id) : TrackingCollar(id)
    {

    }

	void QSetTrackingCollar::askPeer(Peer::pointer peer)
	{
		peer->sendGetQuorumSet(mItemID);
	}

    TxSetTrackingCollar::TxSetTrackingCollar(stellarxdr::uint256& id) : TrackingCollar(id)
    {

    }

	void TxSetTrackingCollar::askPeer(Peer::pointer peer)
	{
		peer->sendGetTxSet(mItemID);
	}
}
