// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/OverlayManagerImpl.h"
#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "database/Database.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/PeerBareAddress.h"
#include "overlay/PeerManager.h"
#include "overlay/TCPPeer.h"
#include "util/Logging.h"
#include "util/XDROperators.h"

#include "medida/counter.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"

#include <algorithm>
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
    return std::make_unique<OverlayManagerImpl>(app);
}

OverlayManagerImpl::OverlayManagerImpl(Application& app)
    : mApp(app)
    , mPeerManager(app)
    , mDoor(mApp)
    , mAuth(mApp)
    , mShuttingDown(false)
    , mMessagesBroadcast(app.getMetrics().NewMeter(
          {"overlay", "message", "broadcast"}, "message"))
    , mConnectionsAttempted(app.getMetrics().NewMeter(
          {"overlay", "connection", "outbound-start"}, "connection"))
    , mConnectionsEstablished(app.getMetrics().NewMeter(
          {"overlay", "connection", "establish"}, "connection"))
    , mConnectionsDropped(app.getMetrics().NewMeter(
          {"overlay", "connection", "drop"}, "connection"))
    , mConnectionsRejected(app.getMetrics().NewMeter(
          {"overlay", "connection", "reject"}, "connection"))
    , mPendingPeersSize(
          app.getMetrics().NewCounter({"overlay", "connection", "pending"}))
    , mAuthenticatedPeersSize(app.getMetrics().NewCounter(
          {"overlay", "connection", "authenticated"}))
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
            [this]() {
                storeConfigPeers();
                this->tick();
            },
            VirtualTimer::onFailureNoop);
    }
}

void
OverlayManagerImpl::connectTo(PeerBareAddress const& address)
{
    mConnectionsAttempted.Mark();

    auto currentConnection = getConnectedPeer(address);
    if (!currentConnection)
    {
        getPeerManager().update(address, PeerManager::BackOffUpdate::INCREASE);

        if (getPendingPeersCount() < mApp.getConfig().MAX_PENDING_CONNECTIONS)
        {
            addPendingPeer(TCPPeer::initiate(mApp, address));
        }
        else
        {
            CLOG(DEBUG, "Overlay")
                << "reached maximum number of pending connections, backing off "
                << address.toString();
        }
    }
    else
    {
        CLOG(ERROR, "Overlay")
            << "trying to connect to a node we're already connected to "
            << address.toString();
    }
}

void
OverlayManagerImpl::storePeerList(std::vector<std::string> const& list,
                                  bool setPreferred)
{
    auto typeUpgrade = setPreferred ? PeerManager::TypeUpdate::SET_PREFERRED
                                    : PeerManager::TypeUpdate::KEEP;

    for (auto const& peerStr : list)
    {
        try
        {
            auto address = PeerBareAddress::resolve(peerStr, mApp);
            getPeerManager().update(address, typeUpgrade,
                                    PeerManager::BackOffUpdate::RESET);
        }
        catch (std::runtime_error&)
        {
            CLOG(ERROR, "Overlay") << "Unable to add peer '" << peerStr << "'";
        }
    }
}

void
OverlayManagerImpl::storeConfigPeers()
{
    // compute normalized mPreferredPeers
    std::vector<std::string> ppeers;
    for (auto const& s : mApp.getConfig().PREFERRED_PEERS)
    {
        try
        {
            auto pr = PeerBareAddress::resolve(s, mApp);
            auto r = mPreferredPeers.insert(pr.toString());
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

    storePeerList(mApp.getConfig().KNOWN_PEERS, false);
    storePeerList(ppeers, true);
}

std::vector<PeerBareAddress>
OverlayManagerImpl::getPreferredPeersFromConfig()
{
    std::vector<PeerBareAddress> peers;
    for (auto& pp : mPreferredPeers)
    {
        auto address = PeerBareAddress::resolve(pp, mApp);
        if (!getConnectedPeer(address))
        {
            auto pr = getPeerManager().load(address);
            if (VirtualClock::tmToPoint(pr.first.mNextAttempt) <=
                mApp.getClock().now())
            {
                peers.emplace_back(address);
            }
        }
    }
    return peers;
}

std::vector<PeerBareAddress>
OverlayManagerImpl::getPeersToConnectTo(int maxNum)
{
    // don't connect to too many peers at once
    maxNum = std::min(maxNum, 50);

    // batch is how many peers to load from the database every time
    const int batchSize = std::max(50, maxNum);

    std::vector<PeerBareAddress> peers;

    getPeerManager().loadPeers(
        batchSize, mApp.getClock().now(), [&](PeerBareAddress const& address) {
            // skip peers that we're already
            // connected/connecting to
            if (!getConnectedPeer(address))
            {
                peers.emplace_back(address);
            }
            return peers.size() < static_cast<size_t>(maxNum);
        });
    orderByPreferredPeers(peers);
    return peers;
}

void
OverlayManagerImpl::connectTo(std::vector<PeerBareAddress> const& peers)
{
    for (auto& address : peers)
    {
        connectTo(address);
    }
}

void
OverlayManagerImpl::orderByPreferredPeers(std::vector<PeerBareAddress>& peers)
{
    auto isPreferredPredicate = [this](PeerBareAddress const& address) -> bool {
        return mPreferredPeers.find(address.toString()) !=
               mPreferredPeers.end();
    };
    std::stable_partition(peers.begin(), peers.end(), isPreferredPredicate);
}

// called every 2 seconds
void
OverlayManagerImpl::tick()
{
    CLOG(TRACE, "Overlay") << "OverlayManagerImpl tick";

    mLoad.maybeShedExcessLoad(mApp);

    // first, see if we should trigger connections to preferred peers
    connectTo(getPreferredPeersFromConfig());

    if (getAuthenticatedPeersCount() < mApp.getConfig().TARGET_PEER_CONNECTIONS)
    {
        // load best candidates from the database,
        // when PREFERRED_PEER_ONLY is set and we connect to a non
        // preferred_peer we just end up dropping & backing off
        // it during handshake (this allows for preferred_peers
        // to work for both ip based and key based preferred mode)
        auto peers = getPeersToConnectTo(
            static_cast<int>(mApp.getConfig().TARGET_PEER_CONNECTIONS -
                             getAuthenticatedPeersCount()));
        connectTo(peers);
    }

    mTimer.expires_from_now(
        std::chrono::seconds(mApp.getConfig().PEER_AUTHENTICATION_TIMEOUT + 1));
    mTimer.async_wait([this]() { this->tick(); }, VirtualTimer::onFailureNoop);
}

Peer::pointer
OverlayManagerImpl::getConnectedPeer(PeerBareAddress const& address)
{
    auto pendingPeerIt =
        std::find_if(std::begin(mPendingPeers), std::end(mPendingPeers),
                     [address](Peer::pointer const& peer) {
                         return peer->getAddress() == address;
                     });
    if (pendingPeerIt != std::end(mPendingPeers))
    {
        return *pendingPeerIt;
    }

    auto authenticatedPeerIt = std::find_if(
        std::begin(mAuthenticatedPeers), std::end(mAuthenticatedPeers),
        [address](std::pair<NodeID, Peer::pointer> const& peer) {
            return peer.second->getAddress() == address;
        });
    if (authenticatedPeerIt != std::end(mAuthenticatedPeers))
    {
        return authenticatedPeerIt->second;
    }

    return Peer::pointer();
}

void
OverlayManagerImpl::ledgerClosed(uint32_t lastClosedledgerSeq)
{
    mFloodGate.clearBelow(lastClosedledgerSeq);
}

void
OverlayManagerImpl::updateSizeCounters()
{
    mPendingPeersSize.set_count(getPendingPeersCount());
    mAuthenticatedPeersSize.set_count(getAuthenticatedPeersCount());
}

void
OverlayManagerImpl::addPendingPeer(Peer::pointer peer)
{
    if (mShuttingDown ||
        getPendingPeersCount() >= mApp.getConfig().MAX_PENDING_CONNECTIONS)
    {
        mConnectionsRejected.Mark();
        peer->drop(Peer::DropMode::IGNORE_WRITE_QUEUE);
        return;
    }
    CLOG(INFO, "Overlay") << "New connected peer " << peer->toString();
    mConnectionsEstablished.Mark();
    mPendingPeers.push_back(peer);
    updateSizeCounters();
}

void
OverlayManagerImpl::removePeer(Peer* peer)
{
    bool dropped = false;
    CLOG(INFO, "Overlay") << "Dropping peer "
                          << mApp.getConfig().toShortString(peer->getPeerID())
                          << "@" << peer->toString();
    auto pendingIt =
        std::find_if(std::begin(mPendingPeers), std::end(mPendingPeers),
                     [&](Peer::pointer const& p) { return p.get() == peer; });
    if (pendingIt != std::end(mPendingPeers))
    {
        mPendingPeers.erase(pendingIt);
        dropped = true;
    }
    else
    {
        auto authentiatedIt = mAuthenticatedPeers.find(peer->getPeerID());
        if (authentiatedIt != std::end(mAuthenticatedPeers))
        {
            mAuthenticatedPeers.erase(authentiatedIt);
            dropped = true;
        }
        else
        {
            CLOG(WARNING, "Overlay") << "Dropping unlisted peer";
        }
    }
    if (dropped)
    {
        mConnectionsDropped.Mark();
    }
    updateSizeCounters();
}

bool
OverlayManagerImpl::moveToAuthenticated(Peer::pointer peer)
{
    auto pendingIt =
        std::find(std::begin(mPendingPeers), std::end(mPendingPeers), peer);
    if (pendingIt == std::end(mPendingPeers))
    {
        CLOG(WARNING, "Overlay")
            << "Trying to move non-pending peer " << peer->toString()
            << " to authenticated list";
        return false;
    }

    auto authenticatedIt = mAuthenticatedPeers.find(peer->getPeerID());
    if (authenticatedIt != std::end(mAuthenticatedPeers))
    {
        CLOG(WARNING, "Overlay")
            << "Trying to move authenticated peer " << peer->toString()
            << " to authenticated list again";
        return false;
    }

    mPendingPeers.erase(pendingIt);
    mAuthenticatedPeers[peer->getPeerID()] = peer;
    updateSizeCounters();
    return true;
}

bool
OverlayManagerImpl::acceptAuthenticatedPeer(Peer::pointer peer)
{
    if (isPreferred(peer.get()))
    {
        if (getAuthenticatedPeersCount() <
            mApp.getConfig().MAX_PEER_CONNECTIONS)
        {
            return moveToAuthenticated(peer);
        }

        for (auto victim : mAuthenticatedPeers)
        {
            if (!isPreferred(victim.second.get()))
            {
                CLOG(INFO, "Overlay")
                    << "Evicting non-preferred peer "
                    << victim.second->toString() << " for preferred peer "
                    << peer->toString();
                victim.second->drop(ERR_LOAD, "preferred peer selected instead",
                                    Peer::DropMode::IGNORE_WRITE_QUEUE);
                return moveToAuthenticated(peer);
            }
        }
    }

    if (!mApp.getConfig().PREFERRED_PEERS_ONLY &&
        getAuthenticatedPeersCount() < mApp.getConfig().MAX_PEER_CONNECTIONS)
    {
        return moveToAuthenticated(peer);
    }

    mConnectionsRejected.Mark();
    return false;
}

std::vector<Peer::pointer> const&
OverlayManagerImpl::getPendingPeers() const
{
    return mPendingPeers;
}

std::map<NodeID, Peer::pointer> const&
OverlayManagerImpl::getAuthenticatedPeers() const
{
    return mAuthenticatedPeers;
}

int
OverlayManagerImpl::getPendingPeersCount() const
{
    return static_cast<int>(mPendingPeers.size());
}

int
OverlayManagerImpl::getAuthenticatedPeersCount() const
{
    return static_cast<int>(mAuthenticatedPeers.size());
}

bool
OverlayManagerImpl::isPreferred(Peer* peer)
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

std::vector<Peer::pointer>
OverlayManagerImpl::getRandomAuthenticatedPeers()
{
    auto goodPeers = std::vector<Peer::pointer>{};
    std::transform(std::begin(mAuthenticatedPeers),
                   std::end(mAuthenticatedPeers), std::back_inserter(goodPeers),
                   [](std::pair<NodeID, Peer::pointer> const& peer) {
                       return peer.second;
                   });
    std::random_shuffle(goodPeers.begin(), goodPeers.end());
    return goodPeers;
}

void
OverlayManagerImpl::recvFloodedMsg(StellarMessage const& msg,
                                   Peer::pointer peer)
{
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
    PeerManager::dropAll(db);
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

PeerManager&
OverlayManagerImpl::getPeerManager()
{
    return mPeerManager;
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
    auto pendingPeersToStop = mPendingPeers;
    for (auto& p : pendingPeersToStop)
    {
        p->drop(ERR_MISC, "peer shutdown", Peer::DropMode::IGNORE_WRITE_QUEUE);
    }
    auto authenticatedPeersToStop = mAuthenticatedPeers;
    for (auto& p : authenticatedPeersToStop)
    {
        p.second->drop(ERR_MISC, "peer shutdown",
                       Peer::DropMode::IGNORE_WRITE_QUEUE);
    }
}

bool
OverlayManagerImpl::isShuttingDown() const
{
    return mShuttingDown;
}
}
