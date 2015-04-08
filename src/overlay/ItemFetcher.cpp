// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SHA.h"
#include "main/Application.h"
#include "overlay/ItemFetcher.h"
#include "overlay/OverlayManager.h"
#include "util/Logging.h"

#include "medida/counter.h"
#include "medida/metrics_registry.h"
#include "xdrpp/marshal.h"

#define MS_TO_WAIT_FOR_FETCH_REPLY 500

// TODO.1 I think we need to add something that after some time it retries to
// fetch qsets that it really needs.
// (https://github.com/stellar/stellar-core/issues/81)
/*

*/

namespace stellar
{

ItemFetcher::ItemFetcher(Application& app)
    : mApp(app)
    , mItemMapSize(
          app.getMetrics().NewCounter({"overlay", "memory", "item-fetch-map"}))
{
}

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
    stopFetchingPred(nullptr);
}

void
ItemFetcher::stopFetchingPred(
    std::function<bool(uint256 const& itemID)> const& pred)
{
    for (auto& item : mItemMap)
    {
        if (!pred || pred(item.first))
            item.second->cancelFetch();
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
    int64_t n = static_cast<int64_t>(mItemMap.size());
    mItemMap.clear();
    mItemMapSize.dec(n);
}

//////////////////////////////

TxSetFramePtr
TxSetFetcher::fetchItem(uint256 const& txSetHash, bool askNetwork)
{
    // look it up in the map
    // if not found then start fetching
    auto result = mItemMap.find(txSetHash);
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
                std::make_shared<TxSetTrackingCollar>(txSetHash,
                                                      TxSetFramePtr(), mApp);
            mItemMap[txSetHash] = collar;
            mItemMapSize.inc();
            collar->tryNextPeer();
        }
    }
    return (TxSetFramePtr());
}

// TODO.1: This all needs to change
// returns true if we were waiting for this txSet
bool
TxSetFetcher::recvItem(TxSetFramePtr txSet)
{
    if (txSet)
    {
        auto result = mItemMap.find(txSet->getContentsHash());
        if (result != mItemMap.end())
        {
            int refCount = result->second->getRefCount();
            result->second->cancelFetch();
            ((TxSetTrackingCollar*)result->second.get())->mTxSet = txSet;
            if (refCount)
            { // someone was still interested in
                // this tx set so tell SCP
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
            mItemMapSize.inc();
        }
    }
    return false;
}

////////////////////////////////////////

SCPQuorumSetPtr
SCPQSetFetcher::fetchItem(uint256 const& qSetHash, bool askNetwork)
{
    // look it up in the map
    // if not found then start fetching
    auto result = mItemMap.find(qSetHash);
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
                std::make_shared<QSetTrackingCollar>(qSetHash,
                                                     SCPQuorumSetPtr(), mApp);
            mItemMap[qSetHash] = collar;
            mItemMapSize.inc();
            collar->tryNextPeer(); // start asking
        }
    }
    return (SCPQuorumSetPtr());
}

// returns true if we were waiting for this qSet
bool
SCPQSetFetcher::recvItem(SCPQuorumSetPtr qSet)
{
    if (qSet)
    {
        uint256 qSetHash = sha256(xdr::xdr_to_opaque(*qSet));
        auto result = mItemMap.find(qSetHash);
        if (result != mItemMap.end())
        {
            int refCount = result->second->getRefCount();
            result->second->cancelFetch();
            ((QSetTrackingCollar*)result->second.get())->mQSet = qSet;
            if (refCount)
            { // someone was still interested in
                // this quorum set so tell SCP
                // LATER: maybe change this to pub/sub
                return true;
            }
        }
        else
        { // doesn't seem like we were looking for it. Maybe just add it for
            // now
            mItemMap[qSetHash] =
                std::make_shared<QSetTrackingCollar>(qSetHash, qSet, mApp);
            mItemMapSize.inc();
        }
    }
    return false;
}

//////////////////////////////////////////////////////////////////////////

TrackingCollar::TrackingCollar(uint256 const& id, Application& app)
    : mApp(app), mTimer(app), mItemID(id)
{
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
                peer = mApp.getOverlayManager().getNextPeer(
                    mPeersAsked[mPeersAsked.size() - 1]);
                if (!peer)
                    mPeersAsked.pop_back();
            }
        }
        else
        {
            peer = mApp.getOverlayManager().getRandomPeer();
        }

        if (peer)
        {
            if (find(mPeersAsked.begin(), mPeersAsked.end(), peer) ==
                mPeersAsked.end())
            { // we have never asked this guy
                mLastAskedPeer = peer;

                askPeer(peer);
                mPeersAsked.push_back(peer);
            }

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
        }
        else
        { // we have asked all our peers
            // clear list and try again in a bit
            mPeersAsked.clear();
            mTimer.cancel(); // cancel any stray timers
            mTimer.expires_from_now(
                std::chrono::milliseconds(MS_TO_WAIT_FOR_FETCH_REPLY * 2));
            mTimer.async_wait([this](asio::error_code const& ec)
                              {
                                  if (!ec)
                                  {
                                      this->tryNextPeer();
                                  }
                              });
        }
    }
}

QSetTrackingCollar::QSetTrackingCollar(uint256 const& id, SCPQuorumSetPtr qSet,
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
