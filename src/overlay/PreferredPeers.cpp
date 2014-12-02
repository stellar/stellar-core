#include "overlay/PreferredPeers.h"

namespace stellar
{
    void PreferredPeers::addPreferredPeers(vector<std::string> const& peerList)
    {
        for(auto peerStr : peerList)
        {
            // LATER
        }
    }

    bool PreferredPeers::isPeerPreferred(Peer::pointer peer)
    {
        for(auto peerPair : mPeerList)
        {
            if((peerPair.second == peer->mPort) &&
                (peerPair.first == peer->mIP)) return true;

        }
        return false;
    }
}
