#include "PeerMaster.h"
#include "main/Application.h"
#include <thread>
#include <random>
#include "ledger/Ledger.h"
#include "lib/util/Logging.h"

/*
If we have less than the target number of peers we will try to connect to one out there

What we need timers for:
    To make sure we have enough peers
    For Item fetcher
    Cleaning up hung peers. They never send Hello

*/


namespace stellar
{


    PeerMaster::PeerMaster(Application &app)
        : mApp(app)
        , mDoor(mApp)
        , mQSetFetcher(mApp)
        , mTimer(app.getMainIOService(), std::chrono::seconds(1))
    {
        mPreferredPeers.addPreferredPeers(mApp.mConfig.PREFERRED_PEERS);
        if(!mApp.mConfig.RUN_STANDALONE) {
            addConfigPeers();
            mTimer.async_wait([this](asio::error_code const& ec){ this->tick(); });
        }
    }

    PeerMaster::~PeerMaster()
    {
    }

    // called every second
    void PeerMaster::tick()
    {
        // if we have too few peers try to connect to more
        LOG(DEBUG) << "PeerMaster tick";
        if(mPeers.size() < mApp.mConfig.TARGET_PEER_CONNECTIONS)
        {
            // LATER
        }
        mTimer.expires_from_now(std::chrono::seconds(1));
        mTimer.async_wait([this](asio::error_code const& ec){ this->tick(); });
    }

    void PeerMaster::ledgerClosed(LedgerPtr ledger)
    {
        mFloodGate.clearBelow(ledger->mLedgerSeq);
    }

    void PeerMaster::addPeer(Peer::pointer peer)
    {
        mPeers.push_back(peer);
    }

    void PeerMaster::dropPeer(Peer::pointer peer)
    {
        auto iter=find(mPeers.begin(), mPeers.end(), peer);
        if(iter != mPeers.end()) mPeers.erase(iter);
        else CLOG(WARNING, "Overlay") << "Dropping unlisted peer";
    }

    bool PeerMaster::isPeerAccepted(Peer::pointer peer)
    {
        if(mPeers.size() < mApp.mConfig.MAX_PEER_CONNECTIONS) return true;
        return mPreferredPeers.isPeerPreferred(peer);
    }

    Peer::pointer PeerMaster::getRandomPeer()
    {
        if(mPeers.size())
        {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(0, mPeers.size()-1);
            return mPeers[dis(gen)];
        }

        return Peer::pointer();
    }

    // returns NULL if the passed peer isn't found
    Peer::pointer PeerMaster::getNextPeer(Peer::pointer peer)
    {
        for(unsigned int n = 0; n < mPeers.size(); n++)
        {
            if(mPeers[n]==peer)
            {
                if(n == mPeers.size() - 1) return mPeers[0];
                return(mPeers[n + 1]);
            }
        }
        return Peer::pointer();
    }

    void PeerMaster::recvQuorumSet(QuorumSet::pointer qset)
    {
        mQSetFetcher.recvItem(qset);
    }

    void PeerMaster::addConfigPeers()
    {
        mPreferredPeers.addPreferredPeers(mApp.mConfig.PREFERRED_PEERS);
    }

    void PeerMaster::broadcastMessage(stellarxdr::uint256& msgID)
    {
        mFloodGate.broadcast(msgID,this);
    }

    void PeerMaster::broadcastMessage(StellarMessagePtr msg, Peer::pointer peer)
    {
        vector<Peer::pointer> tempList;
        tempList.push_back(peer);
        broadcastMessage(msg, tempList);
    }

    // send message to anyone you haven't gotten it from
    void PeerMaster::broadcastMessage(StellarMessagePtr msg, vector<Peer::pointer>& skip)
    {
        for(auto peer: mPeers)
        {
            if(find(skip.begin(), skip.end(), peer) == skip.end())
            {
                peer->sendMessage(*msg);
            }
        }
    }
}

