#ifndef __ITEMFETCHER__
#define __ITEMFETCHER__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <map>
#include "generated/FBAXDR.h"
#include "overlay/OverlayGateway.h"
#include "herder/TxSetFrame.h"
#include "overlay/Peer.h"
#include "util/Timer.h"

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
    Application& mApp;
    Peer::pointer mLastAskedPeer;
    vector<Peer::pointer> mPeersAsked;
    bool mCantFind;
    VirtualTimer mTimer;
    int mRefCount;

  protected:
    virtual void askPeer(Peer::pointer peer) = 0;

  public:
    typedef std::shared_ptr<TrackingCollar> pointer;

    uint256 mItemID;

    virtual bool isItemFound() = 0;

    TrackingCollar(uint256 const& id, Application& app);

    void doesntHave(Peer::pointer peer);
    void tryNextPeer();
    void cancelFetch();
    void
    refInc()
    {
        mRefCount++;
    }
    void refDec();
    int
    getRefCount()
    {
        return mRefCount;
    }
};

class ItemFetcher
{
  protected:
    Application& mApp;
    std::map<uint256, TrackingCollar::pointer> mItemMap;

  public:
    ItemFetcher(Application& app) : mApp(app)
    {
    }
    void clear();
    void stopFetching(uint256 const& itemID);
    void stopFetchingAll();
    void doesntHave(uint256 const& itemID, Peer::pointer peer);
};

// We want to keep the last N ledgers worth of Txsets around
//    in case there are stragglers still trying to close
class TxSetFetcher : public ItemFetcher
{
  public:
    TxSetFetcher(Application& app) : ItemFetcher(app)
    {
    }
    TxSetFramePtr fetchItem(uint256 const& itemID,
                                      bool askNetwork);
    // looks to see if we know about it but doesn't ask the network
    TxSetFramePtr findItem(uint256 const& itemID);
    bool recvItem(TxSetFramePtr txSet);
};

class FBAQSetFetcher : public ItemFetcher
{
  public:
    FBAQSetFetcher(Application& app) : ItemFetcher(app)
    {
    }
    FBAQuorumSetPtr fetchItem(uint256 const& itemID,
                                 bool askNetwork);
    // looks to see if we know about it but doesn't ask the network
    FBAQuorumSetPtr findItem(uint256 const& itemID);
    bool recvItem(FBAQuorumSetPtr qSet);
};

class TxSetTrackingCollar : public TrackingCollar
{

    void askPeer(Peer::pointer peer);

  public:
    TxSetFramePtr mTxSet;

    TxSetTrackingCollar(uint256 const& id, Application& app);
    bool
    isItemFound()
    {
        return (!!mTxSet);
    }
};

class QSetTrackingCollar : public TrackingCollar
{
    void askPeer(Peer::pointer peer);

  public:
    FBAQuorumSetPtr mQSet;

    QSetTrackingCollar(uint256 const& id, Application& app);
    bool
    isItemFound()
    {
        return (!!mQSet);
    }
};
}

#endif
