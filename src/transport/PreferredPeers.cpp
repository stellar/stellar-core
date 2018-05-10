// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transport/PreferredPeers.h"
#include "crypto/KeyUtils.h"
#include "main/Application.h"
#include "main/Config.h"
#include "util/Logging.h"

namespace stellar
{
PreferredPeers::PreferredPeers(Application& app) : mApp{app}
{
}

PreferredPeers::~PreferredPeers()
{
}

std::set<std::string> const&
PreferredPeers::getAll() const
{
    return mPreferredPeers;
}

bool
PreferredPeers::isPreferred(Peer* peer)
{
    std::string pstr = peer->toString();

    if (mPreferredPeers.find(pstr) != mPreferredPeers.end())
    {
        CLOG(DEBUG, "Overlay") << "Peer " << pstr << " is preferred";
        return true;
    }

    if (peer->isAuthenticated())
    {
        std::string kstr = KeyUtils::toStrKey(peer->getPeerID());
        std::vector<std::string> const& pk =
            mApp.getConfig().PREFERRED_PEER_KEYS;
        if (std::find(pk.begin(), pk.end(), kstr) != pk.end())
        {
            CLOG(DEBUG, "Overlay")
                << "Peer key " << mApp.getConfig().toStrKey(peer->getPeerID())
                << " is preferred";
            return true;
        }
    }

    CLOG(DEBUG, "Overlay") << "Peer " << pstr << " is not preferred";
    return false;
}

void
PreferredPeers::orderByPreferredPeers(vector<PeerRecord>& peers)
{
    auto isPreferredPredicate = [this](PeerRecord& record) -> bool {
        return mPreferredPeers.find(record.toString()) != mPreferredPeers.end();
    };
    std::stable_partition(peers.begin(), peers.end(), isPreferredPredicate);
}

void
PreferredPeers::storeConfigPeers()
{
    // compute normalized mPreferredPeers
    std::vector<std::string> ppeers;
    for (auto const& s : mApp.getConfig().PREFERRED_PEERS)
    {
        try
        {
            auto address = PeerBareAddress::resolve(s, mApp);
            auto r = mPreferredPeers.insert(address.toString());
            if (r.second)
            {
                ppeers.push_back(*r.first);
            }
        }
        catch (std::runtime_error&)
        {
            CLOG(ERROR, "Overlay")
                << "Unable to add preferred peer '" << s << "'";
        }
    }

    storePeerList(mApp.getConfig().KNOWN_PEERS, true, false);
    storePeerList(ppeers, true, true);
}

void
PreferredPeers::storePeerList(std::vector<std::string> const& list,
                              bool resetBackOff, bool preferred)
{
    for (auto const& peerStr : list)
    {
        try
        {
            auto address = PeerBareAddress::resolve(peerStr, mApp);
            auto pr = PeerRecord{address, mApp.getClock().now(), 0};
            pr.setPreferred(preferred);
            if (resetBackOff)
            {
                pr.storePeerRecord(mApp.getDatabase());
            }
            else
            {
                pr.insertIfNew(mApp.getDatabase());
            }
        }
        catch (std::runtime_error&)
        {
            CLOG(ERROR, "Overlay") << "Unable to add peer '" << peerStr << "'";
        }
    }
}
}
