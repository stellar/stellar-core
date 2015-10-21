// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SecretKey.h"
#include "database/Database.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/OverlayManagerImpl.h"
#include "overlay/PeerRecord.h"
#include "overlay/TCPPeer.h"
#include "util/Logging.h"
#include "util/make_unique.h"

#include "medida/metrics_registry.h"
#include "medida/meter.h"
#include "medida/counter.h"

#include <random>

/*

Connection process:
A wants to connect to B
A initiates a tcp connection to B
connection is established
A sends HELLO(CertA,NonceA) to B
B now has IP and listening port of A, sends HELLO(CertB,NonceB) back
A sends AUTH(signed([0],keyAB))
B verifies and either:
    sends AUTH(signed([0],keyBA)) back or
    disconnects, if it's full, optionally sending a list of other peers to try
first

keyAB and keyBA are per-connection HMAC keys derived from non-interactive
ECDH on random curve25519 keys conveyed in CertA and CertB (certs signed by
Node Ed25519 keys) the result of which is then fed through HKDF with the
per-connection nonces. See PeerAuth.h.

If any verify step fails, the peer disconnects immediately.

*/

namespace stellar
{

using namespace soci;
using namespace std;

std::unique_ptr<OverlayManager>
OverlayManager::create(Application& app)
{
    return make_unique<OverlayManagerImpl>(app);
}

OverlayManagerImpl::OverlayManagerImpl(Application& app)
    : mApp(app)
    , mDoor(mApp)
    , mAuth(mApp)
    , mShuttingDown(false)
    , mMessagesReceived(app.getMetrics().NewMeter(
          {"overlay", "message", "flood-receive"}, "message"))
    , mMessagesBroadcast(app.getMetrics().NewMeter(
          {"overlay", "message", "broadcast"}, "message"))
    , mConnectionsAttempted(app.getMetrics().NewMeter(
          {"overlay", "connection", "attempt"}, "connection"))
    , mConnectionsEstablished(app.getMetrics().NewMeter(
          {"overlay", "connection", "establish"}, "connection"))
    , mConnectionsDropped(app.getMetrics().NewMeter(
          {"overlay", "connection", "drop"}, "connection"))
    , mConnectionsRejected(app.getMetrics().NewMeter(
          {"overlay", "connection", "reject"}, "connection"))
    , mPeersSize(app.getMetrics().NewCounter({"overlay", "memory", "peers"}))
    , mTimer(app)
    , mFloodGate(app)
{
}

OverlayManagerImpl::~OverlayManagerImpl()
{
}

void
OverlayManagerImpl::start()
{
    mDoor.start();
    mTimer.expires_from_now(std::chrono::seconds(2));

    if (!mApp.getConfig().RUN_STANDALONE)
    {
        mTimer.async_wait(
            [this]()
            {
                storeConfigPeers();
                this->tick();
            },
            VirtualTimer::onFailureNoop);
    }
}

void
OverlayManagerImpl::connectTo(std::string const& peerStr)
{
    PeerRecord pr;
    if (PeerRecord::parseIPPort(peerStr, mApp, pr))
        connectTo(pr);
    else
    {
        CLOG(ERROR, "Overlay") << "Unable to add peer '" << peerStr << "'";
    }
}

void
OverlayManagerImpl::connectTo(PeerRecord& pr)
{
    if (pr.mPort == 0)
    {
        CLOG(ERROR, "Overlay") << "Invalid port: " << pr.toString();
        return;
    }

    if (!pr.mIP.size())
    {
        CLOG(ERROR, "Overlay") << "OverlayManagerImpl::connectTo Invalid IP ";
        return;
    }

    mConnectionsAttempted.Mark();
    if (!getConnectedPeer(pr.mIP, pr.mPort))
    {
        pr.backOff(mApp.getClock());
        pr.storePeerRecord(mApp.getDatabase());

        addConnectedPeer(TCPPeer::initiate(mApp, pr.mIP, pr.mPort));
    }
    else
    {
        CLOG(ERROR, "Overlay")
            << "trying to connect to a node we're already connected to"
            << pr.toString();
    }
}

void
OverlayManagerImpl::storePeerList(std::vector<std::string> const& list)
{
    for (auto const& peerStr : list)
    {
        PeerRecord pr;
        if (PeerRecord::parseIPPort(peerStr, mApp, pr))
        {
            pr.insertIfNew(mApp.getDatabase());
        }
        else
        {
            CLOG(ERROR, "Overlay") << "Unable to add peer '" << peerStr << "'";
        }
    }
}

void
OverlayManagerImpl::storeConfigPeers()
{
    storePeerList(mApp.getConfig().KNOWN_PEERS);
    storePeerList(mApp.getConfig().PREFERRED_PEERS);
}

void
OverlayManagerImpl::connectToMorePeers(int max)
{
    vector<PeerRecord> peers;

    // Always retry the preferred peers before anything else.
    for (auto const& peerStr : mApp.getConfig().PREFERRED_PEERS)
    {
        PeerRecord pr;
        if (PeerRecord::parseIPPort(peerStr, mApp, pr))
        {
            // see if we can get current information from the peer table
            auto prFromDB = PeerRecord::loadPeerRecord(mApp.getDatabase(),
                                                       pr.mIP, pr.mPort);
            if (prFromDB)
            {
                pr = *prFromDB;
            }
            peers.push_back(pr);
        }
    }

    // Load additional peers from the DB if we're not in whitelist mode.
    if (!mApp.getConfig().PREFERRED_PEERS_ONLY)
    {
        PeerRecord::loadPeerRecords(mApp.getDatabase(), max,
                                    mApp.getClock().now(), peers);
    }

    for (auto& pr : peers)
    {
        if (pr.mNextAttempt > mApp.getClock().now())
        {
            continue;
        }
        if (mPeers.size() >= mApp.getConfig().TARGET_PEER_CONNECTIONS)
        {
            break;
        }
        if (!getConnectedPeer(pr.mIP, pr.mPort))
        {
            connectTo(pr);
        }
    }
}

// called every 2 seconds
void
OverlayManagerImpl::tick()
{
    CLOG(TRACE, "Overlay") << "OverlayManagerImpl tick";

    mLoad.maybeShedExcessLoad(mApp);

    if (mPeers.size() < mApp.getConfig().TARGET_PEER_CONNECTIONS)
    {
        connectToMorePeers(static_cast<int>(
            mApp.getConfig().TARGET_PEER_CONNECTIONS - mPeers.size()));
    }

    mTimer.expires_from_now(std::chrono::seconds(2));
    mTimer.async_wait(
        [this]()
        {
            this->tick();
        },
        VirtualTimer::onFailureNoop);
}

Peer::pointer
OverlayManagerImpl::getConnectedPeer(std::string const& ip, unsigned short port)
{
    for (auto& peer : mPeers)
    {
        if (peer->getIP() == ip && peer->getRemoteListeningPort() == port)
        {
            return peer;
        }
    }
    return Peer::pointer();
}

void
OverlayManagerImpl::ledgerClosed(uint32_t lastClosedledgerSeq)
{
    mFloodGate.clearBelow(lastClosedledgerSeq);
}

void
OverlayManagerImpl::addConnectedPeer(Peer::pointer peer)
{
    if (mShuttingDown)
    {
        peer->drop();
        return;
    }
    CLOG(INFO, "Overlay") << "New connected peer " << peer->toString();
    mConnectionsEstablished.Mark();
    mPeers.push_back(peer);
    mPeersSize.set_count(mPeers.size());
}

void
OverlayManagerImpl::dropPeer(Peer::pointer peer)
{
    mConnectionsDropped.Mark();
    CLOG(INFO, "Overlay") << "Dropping peer "
                          << mApp.getConfig().toShortString(peer->getPeerID())
                          << "@" << peer->toString();
    auto iter = find(mPeers.begin(), mPeers.end(), peer);
    if (iter != mPeers.end())
        mPeers.erase(iter);
    else
        CLOG(WARNING, "Overlay") << "Dropping unlisted peer";
    mPeersSize.set_count(mPeers.size());
}

bool
OverlayManagerImpl::isPeerAccepted(Peer::pointer peer)
{
    if (isPeerPreferred(peer))
    {
        if (mPeers.size() < mApp.getConfig().MAX_PEER_CONNECTIONS)
        {
            return true;
        }

        for (auto victim : mPeers)
        {
            if (!isPeerPreferred(victim))
            {
                CLOG(INFO, "Overlay")
                    << "Evicting non-preferred peer " << victim->toString()
                    << " for preferred peer " << peer->toString();
                dropPeer(victim);
                return true;
            }
        }
    }

    if (!mApp.getConfig().PREFERRED_PEERS_ONLY &&
        mPeers.size() < mApp.getConfig().MAX_PEER_CONNECTIONS)
        return true;

    mConnectionsRejected.Mark();
    return false;
}

std::vector<Peer::pointer>&
OverlayManagerImpl::getPeers()
{
    return mPeers;
}

bool
OverlayManagerImpl::isPeerPreferred(Peer::pointer peer)
{
    std::string pstr = peer->toString();
    std::vector<std::string> const& pp = mApp.getConfig().PREFERRED_PEERS;

    if (std::find(pp.begin(), pp.end(), pstr) != pp.end())
    {
        CLOG(DEBUG, "Overlay") << "Peer " << pstr << " is preferred";
        return true;
    }

    if (peer->isAuthenticated())
    {
        std::string kstr = PubKeyUtils::toStrKey(peer->getPeerID());
        std::vector<std::string> const& pk =
            mApp.getConfig().PREFERRED_PEER_KEYS;
        if (std::find(pk.begin(), pk.end(), kstr) != pk.end())
        {
            CLOG(DEBUG, "Overlay") << "Peer key " << 
                mApp.getConfig().toStrKey(peer->getPeerID()) << " is preferred";
            return true;
        }
    }

    CLOG(DEBUG, "Overlay") << "Peer " << pstr << " is not preferred";
    return false;
}

std::vector<Peer::pointer>
OverlayManagerImpl::getRandomPeers()
{
    std::vector<std::shared_ptr<Peer>> goodPeers(mPeers.size());
    auto it = std::copy_if(mPeers.begin(), mPeers.end(), goodPeers.begin(),
                           [](std::shared_ptr<Peer> const& p)
                           {
                               return p && p->isAuthenticated();
                           });
    goodPeers.resize(std::distance(goodPeers.begin(), it));

    std::random_shuffle(goodPeers.begin(), goodPeers.end());

    return goodPeers;
}

void
OverlayManagerImpl::recvFloodedMsg(StellarMessage const& msg,
                                   Peer::pointer peer)
{
    mMessagesReceived.Mark();
    mFloodGate.addRecord(msg, peer);
}

void
OverlayManagerImpl::broadcastMessage(StellarMessage const& msg, bool force)
{
    mMessagesBroadcast.Mark();
    mFloodGate.broadcast(msg, force);
}

void
OverlayManager::dropAll(Database& db)
{
    PeerRecord::dropAll(db);
}

std::set<Peer::pointer>
OverlayManagerImpl::getPeersKnows(Hash const& h)
{
    return mFloodGate.getPeersKnows(h);
}

PeerAuth&
OverlayManagerImpl::getPeerAuth()
{
    return mAuth;
}

LoadManager&
OverlayManagerImpl::getLoadManager()
{
    return mLoad;
}

void
OverlayManagerImpl::shutdown()
{
    if (mShuttingDown)
    {
        return;
    }
    mShuttingDown = true;
    mDoor.close();
    mFloodGate.shutdown();
    auto peersToStop = mPeers;
    for (auto& p : peersToStop)
    {
        p->drop(ERR_MISC, "peer shutdown");
    }
}

bool
OverlayManagerImpl::isShuttingDown() const
{
    return mShuttingDown;
}
}
