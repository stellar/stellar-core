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
A sends HELLO(nonce-a) to B
B now has IP and listening port of A, sends HELLO(nonce-b) back
A sends AUTH:sig(nonce-a,nonce-b)
B verifies and either:
    sends AUTH:sig(nonce-b,nonce-a) back or
    sends list of other peers to connect to and disconnects, if it's full

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
    , mDoor(make_shared<PeerDoor>(mApp))
    , mShuttingDown(false)
    , mMessagesReceived(app.getMetrics().NewMeter(
          {"overlay", "message", "receive"}, "message"))
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
    mDoor->start();
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
            peers.push_back(pr);
        }
    }

    // Load additional peers from the DB if we're not in whitelist mode.
    if (!mApp.getConfig().PREFERRED_PEERS_ONLY)
    {
        PeerRecord::loadPeerRecords(mApp.getDatabase(), max, mApp.getClock().now(),
                                    peers);
    }

    for (auto& pr : peers)
    {
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
    CLOG(DEBUG, "Overlay") << "Dropping peer " << peer->toString();
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
        std::vector<std::string> const& pk = mApp.getConfig().PREFERRED_PEER_KEYS;
        if (std::find(pk.begin(), pk.end(), kstr) != pp.end())
        {
            CLOG(DEBUG, "Overlay") << "Peer key " << kstr << " is preferred";
            return true;
        }
    }

    CLOG(DEBUG, "Overlay") << "Peer " << pstr << " is not preferred";
    return false;
}

Peer::pointer
OverlayManagerImpl::getRandomPeer()
{
    if (mPeers.size())
    {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<size_t> dis(0, mPeers.size() - 1);
        return mPeers[dis(gen)];
    }

    return Peer::pointer();
}

// returns NULL if the passed peer isn't found
Peer::pointer
OverlayManagerImpl::getNextPeer(Peer::pointer peer)
{
    auto index = std::find(mPeers.begin(), mPeers.end(), peer);
    if (mPeers.empty() || index == mPeers.end())
    {
        return nullptr;
    }
    else if (index + 1 == mPeers.end())
    {
        return mPeers.front();
    }
    else
        return *(index + 1);
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

void
OverlayManagerImpl::shutdown()
{
    if (mShuttingDown)
    {
        return;
    }
    mShuttingDown = true;
    if (mDoor)
    {
        mDoor->close();
    }
    mFloodGate.shutdown();
    auto peersToStop = mPeers;
    for (auto& p : peersToStop)
    {
        p->drop();
    }
}

bool
OverlayManagerImpl::isShuttingDown() const
{
    return mShuttingDown;
}
}
