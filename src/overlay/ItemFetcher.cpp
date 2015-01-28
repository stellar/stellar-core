// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "ItemFetcher.h"
#include "main/Application.h"
#include "xdrpp/marshal.h"
#include "crypto/SHA.h"
#include "overlay/OverlayGateway.h"

#define MS_TO_WAIT_FOR_FETCH_REPLY 3000

/*

*/

namespace stellar
{
void
ItemFetcher::doesntHave(uint256 const& itemID, Peer::pointer peer)
{
    auto result = mItemMap.find(itemID);
    if (result != mItemMap.end())
    { // found
        result->second->doesntHave(peer);
    }
}
void
ItemFetcher::stopFetchingAll()
{
    for (auto result : mItemMap)
    {
        result.second->cancelFetch();
    }
}

// LATER  Do we ever need to call this
void
ItemFetcher::stopFetching(uint256 const& itemID)
{
    auto result = mItemMap.find(itemID);
    if (result != mItemMap.end())
    {
        result->second->refDec();
    }
}

void
ItemFetcher::clear()
{
    mItemMap.clear();
}

//////////////////////////////

TxSetFramePtr
TxSetFetcher::fetchItem(uint256 const& setID, bool askNetwork)
{
    // look it up in the map
    // if not found then start fetching
    auto result = mItemMap.find(setID);
    if (result != mItemMap.end())
    { // collar found
        if (result->second->isItemFound())
        {
            return ((TxSetTrackingCollar*)result->second.get())->mTxSet;
        }
        else
        {
            result->second->refInc();
        }
    }
    else
    { // not found
        if (askNetwork)
        {
            TrackingCollar::pointer collar =
                std::make_shared<TxSetTrackingCollar>(setID, TxSetFramePtr(), mApp);
            mItemMap[setID] = collar;
            collar->tryNextPeer();
        }
    }
    return (TxSetFramePtr());
}

// returns true if we were waiting for this txSet
bool
TxSetFetcher::recvItem(TxSetFramePtr txSet)
{
    if (txSet)
    {
        auto result = mItemMap.find(txSet->getContentsHash());
        if (result != mItemMap.end())
        {
            result->second->cancelFetch();
            ((TxSetTrackingCollar*)result->second.get())->mTxSet = txSet;
            if (result->second->getRefCount())
            { // someone was still interested in
                // this tx set so tell FBA
                // LATER: maybe change this to pub/sub
                return true;
            }
        }
        else
        { // doesn't seem like we were looking for it. Maybe just add it for
            // now
            mItemMap[txSet->getContentsHash()] = 
                std::make_shared<TxSetTrackingCollar>(txSet->getContentsHash(),
                       txSet, mApp);
                
        }
    }
    return false;
}

////////////////////////////////////////
bool
FBAQSetFetcher::recvItem(FBAQuorumSetPtr qSet)
{
    if (qSet)
    {
        uint256 qSetHash = sha512_256(xdr::xdr_to_msg(*qSet));
        auto result = mItemMap.find(qSetHash);
        if (result != mItemMap.end())
        {
            result->second->cancelFetch();
            ((QSetTrackingCollar*)result->second.get())->mQSet = qSet;
            if (result->second->getRefCount())
            { // someone was still interested in
                // this quorum set so tell FBA
                // LATER: maybe change this to pub/sub
                return true;
            }
        }
        else
        { // doesn't seem like we were looking for it. Maybe just add it for
            // now
            mItemMap[qSetHash] =
                std::make_shared<QSetTrackingCollar>(qSetHash, qSet, mApp);
        }
    }
    return false;
}

FBAQuorumSetPtr
FBAQSetFetcher::fetchItem(uint256 const& setID, bool askNetwork)
{
    // look it up in the map
    // if not found then start fetching
    auto result = mItemMap.find(setID);
    if (result != mItemMap.end())
    { // collar found
        if (result->second->isItemFound())
        {
            return ((QSetTrackingCollar*)result->second.get())->mQSet;
        }
        else
        {
            result->second->refInc();
        }
    }
    else
    { // not found
        if (askNetwork)
        {
            TrackingCollar::pointer collar =
                std::make_shared<QSetTrackingCollar>(setID, FBAQuorumSetPtr(), mApp);
            mItemMap[setID] = collar;
            collar->tryNextPeer(); // start asking
        }
    }
    return (FBAQuorumSetPtr());
}

//////////////////////////////////////////////////////////////////////////

TrackingCollar::TrackingCollar(uint256 const& id, Application& app)
    : mApp(app), mTimer(app.getClock()), mItemID(id)
{
    mCantFind = false;
    mRefCount = 1;
}

void
TrackingCollar::doesntHave(Peer::pointer peer)
{
    if (mLastAskedPeer == peer)
    {
        tryNextPeer();
    }
}

void
TrackingCollar::refDec()
{
    mRefCount--;
    if (mRefCount < 1)
        cancelFetch();
}

void
TrackingCollar::cancelFetch()
{
    mRefCount = 0;
    mTimer.cancel();
}

// will be called by some timer or when we get a result saying they don't have
// it
void
TrackingCollar::tryNextPeer()
{
    if (!isItemFound())
    { // we still haven't found this item
        Peer::pointer peer;

        if (mPeersAsked.size())
        {
            while (!peer && mPeersAsked.size())
            { // keep looping till we find a peer
                // we are still connected to
                peer = mApp.getOverlayGateway().getNextPeer(
                    mPeersAsked[mPeersAsked.size() - 1]);
                if (!peer)
                    mPeersAsked.pop_back();
            }
        }
        else
        {
            peer = mApp.getOverlayGateway().getRandomPeer();
        }

        if (peer)
        {
            if (find(mPeersAsked.begin(), mPeersAsked.end(), peer) ==
                mPeersAsked.end())
            { // we have never asked this guy
                mLastAskedPeer = peer;

                mTimer.cancel(); // cancel any stray timers
                mTimer.expires_from_now(
                    std::chrono::milliseconds(MS_TO_WAIT_FOR_FETCH_REPLY));
                mTimer.async_wait([this](asio::error_code const& ec)
                                  {
                                      if (!ec)
                                      {
                                          this->tryNextPeer();
                                      }
                                  });

                askPeer(peer);
                mPeersAsked.push_back(peer);
            }
            else
            { // we have looped back around
                mCantFind = true;
                // LATER what should we do here?
                // try to connect to more peers?
                // just ask any new peers we connect to?
                // wait a longer amount of time and then loop again?
            }
        }
    }
}

QSetTrackingCollar::QSetTrackingCollar(uint256 const& id, FBAQuorumSetPtr qSet,
                                       Application& app)
    : TrackingCollar(id, app), mQSet(qSet)
{
}

void
QSetTrackingCollar::askPeer(Peer::pointer peer)
{
    peer->sendGetQuorumSet(mItemID);
}

TxSetTrackingCollar::TxSetTrackingCollar(uint256 const& id, TxSetFramePtr txSet,
                                         Application& app)
    : TrackingCollar(id, app), mTxSet(txSet)
{
}

void
TxSetTrackingCollar::askPeer(Peer::pointer peer)
{
    peer->sendGetTxSet(mItemID);
}
}
