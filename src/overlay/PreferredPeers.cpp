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
        int port = peer->getRemoteListeningPort();
        std::string const &ip = peer->getIP();
        for(auto peerPair : mPeerList)
        {
            if((peerPair.second == port) &&
               (peerPair.first == ip)) return true;
        }
        return false;
    }
}
