#include "PeerMaster.h"
#include "main/Application.h"
#include <thread>
#include <random>
#include "ledger/Ledger.h"

/*
If we have less than the target number of peers we will try to connect to one out there

*/


namespace stellar
{
    PeerMaster gPeerMaster;

	PeerMaster::PeerMaster()
	{
		
	}

	PeerMaster::~PeerMaster()
	{
		
	}

	void PeerMaster::run()
	{
		while(1)
		{
			mIOservice.run();
			mIOservice.reset();
		}
	}

	void PeerMaster::start()
	{
		if(!gApp.mConfig.RUN_STANDALONE)
		{
			addConfigPeers();

			mDoor.start();

			auto fun = std::bind(&PeerMaster::run, this);
			mPeerThread = std::thread(fun);
		}	
	}

	void PeerMaster::ledgerClosed(LedgerPtr ledger)
	{
		mFloodGate.clearBelow(ledger->mLedgerSeq);
	}

	void PeerMaster::addPeer(Peer::pointer peer)
	{

	}
	void PeerMaster::dropPeer(Peer::pointer peer)
	{

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

	}

	void PeerMaster::broadcastMessage(stellarxdr::uint256& msgID)
	{
		mFloodGate.broadcast(msgID);
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
				peer->sendMessage(msg);
			}
		}
	}
}

