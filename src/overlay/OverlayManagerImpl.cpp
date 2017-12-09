// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/OverlayManagerImpl.h"
#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "database/Database.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/PeerRecord.h"
#include "overlay/TCPPeer.h"
#include "util/Logging.h"
#include "util/make_unique.h"

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
    , mPendingPeersSize(
          app.getMetrics().NewCounter({"overlay", "memory", "pending-peers"}))
    , mAuthenticatedPeersSize(app.getMetrics().NewCounter(
          {"overlay", "memory", "authenticated-peers"}))
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
OverlayManagerImpl::connectTo(std::string const& peerStr)
{
    try
    {
        auto pr = PeerRecord::parseIPPort(peerStr, mApp);
        connectTo(pr);
    }
    catch (const std::runtime_error&)
    {
        CLOG(ERROR, "Overlay") << "Unable to add peer '" << peerStr << "'";
    }
}

void
OverlayManagerImpl::connectTo(PeerRecord& pr)
{
    mConnectionsAttempted.Mark();
    if (!getConnectedPeer(pr.ip(), pr.port()))
    {
        pr.backOff(mApp.getClock());
        pr.storePeerRecord(mApp.getDatabase());

        addPendingPeer(TCPPeer::initiate(mApp, pr.ip(), pr.port()));
    }
    else
    {
        CLOG(ERROR, "Overlay")
            << "trying to connect to a node we're already connected to "
            << pr.toString();
    }
}

void
OverlayManagerImpl::storePeerList(std::vector<std::string> const& list,
                                  bool resetBackOff, bool preferred)
{
    for (auto const& peerStr : list)
    {
        try
        {
            auto pr = PeerRecord::parseIPPort(peerStr, mApp);
            if (resetBackOff)
            {
                pr.resetBackOff(mApp.getClock(), preferred);
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

void
OverlayManagerImpl::storeConfigPeers()
{
    // compute normalized mPreferredPeers
    std::vector<std::string> ppeers;
    for (auto const& s : mApp.getConfig().PREFERRED_PEERS)
    {
        try
        {
            auto pr = PeerRecord::parseIPPort(s, mApp);
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

    storePeerList(mApp.getConfig().KNOWN_PEERS, true, false);
    storePeerList(ppeers, true, true);
}

void
OverlayManagerImpl::connectToMorePeers(int max)
{
    const int batchSize = std::max(10, max);

    // load best candidates from the database,
    // when PREFERRED_PEER_ONLY is set and we connect to a non
    // preferred_peer we just end up dropping & backing off
    // it during handshake (this allows for preferred_peers
    // to work for both ip based and key based preferred mode)

    vector<PeerRecord> peers;

    PeerRecord::loadPeerRecords(mApp.getDatabase(), batchSize,
                                mApp.getClock().now(),
                                [&](PeerRecord const& pr) {
                                    // skip peers that we're already connected
                                    // to
                                    if (!getConnectedPeer(pr.ip(), pr.port()))
                                    {
                                        peers.emplace_back(pr);
                                    }
                                    return peers.size() < max;
                                });

    orderByPreferredPeers(peers);

    for (auto& pr : peers)
    {
        if (pr.mNextAttempt > mApp.getClock().now())
        {
            continue;
        }
        if (getAuthenticatedPeersCount() >=
            mApp.getConfig().TARGET_PEER_CONNECTIONS)
        {
            break;
        }
        connectTo(pr);
    }
}

void
OverlayManagerImpl::orderByPreferredPeers(vector<PeerRecord>& peers)
{
    auto isPreferredPredicate = [this](PeerRecord& record) -> bool {
        return mPreferredPeers.find(record.toString()) != mPreferredPeers.end();
    };
    std::stable_partition(peers.begin(), peers.end(), isPreferredPredicate);
}

// called every 2 seconds
void
OverlayManagerImpl::tick()
{
    CLOG(TRACE, "Overlay") << "OverlayManagerImpl tick";

    mLoad.maybeShedExcessLoad(mApp);

    if (getAuthenticatedPeersCount() < mApp.getConfig().TARGET_PEER_CONNECTIONS)
    {
        connectToMorePeers(
            static_cast<int>(mApp.getConfig().TARGET_PEER_CONNECTIONS -
                             getAuthenticatedPeersCount()));
    }

    mTimer.expires_from_now(
        std::chrono::seconds(mApp.getConfig().PEER_AUTHENTICATION_TIMEOUT + 1));
    mTimer.async_wait([this]() { this->tick(); }, VirtualTimer::onFailureNoop);
}

Peer::pointer
OverlayManagerImpl::getConnectedPeer(std::string const& ip, unsigned short port)
{
    auto pendingPeerIt =
        std::find_if(std::begin(mPendingPeers), std::end(mPendingPeers),
                     [ip, port](Peer::pointer const& peer) {
                         return peer->getIP() == ip &&
                                peer->getRemoteListeningPort() == port;
                     });
    if (pendingPeerIt != std::end(mPendingPeers))
    {
        return *pendingPeerIt;
    }

    auto authenticatedPeerIt = std::find_if(
        std::begin(mAuthenticatedPeers), std::end(mAuthenticatedPeers),
        [ip, port](std::pair<NodeID, Peer::pointer> const& peer) {
            return peer.second->getIP() == ip &&
                   peer.second->getRemoteListeningPort() == port;
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
        peer->drop();
        return;
    }
    CLOG(INFO, "Overlay") << "New connected peer " << peer->toString();
    mConnectionsEstablished.Mark();
    mPendingPeers.push_back(peer);
    updateSizeCounters();
}

void
OverlayManagerImpl::dropPeer(Peer* peer)
{
    mConnectionsDropped.Mark();
    CLOG(INFO, "Overlay") << "Dropping peer "
                          << mApp.getConfig().toShortString(peer->getPeerID())
                          << "@" << peer->toString();
    auto pendingIt =
        std::find_if(std::begin(mPendingPeers), std::end(mPendingPeers),
                     [&](Peer::pointer const& p) { return p.get() == peer; });
    if (pendingIt != std::end(mPendingPeers))
    {
        mPendingPeers.erase(pendingIt);
    }
    else
    {
        auto authentiatedIt = mAuthenticatedPeers.find(peer->getPeerID());
        if (authentiatedIt != std::end(mAuthenticatedPeers))
        {
            mAuthenticatedPeers.erase(authentiatedIt);
        }
        else
        {
            CLOG(WARNING, "Overlay") << "Dropping unlisted peer";
        }
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
                dropPeer(victim.second.get());
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

size_t
OverlayManagerImpl::getPendingPeersCount() const
{
    return mPendingPeers.size();
}

size_t
OverlayManagerImpl::getAuthenticatedPeersCount() const
{
    return mAuthenticatedPeers.size();
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
    auto pendingPeersToStop = mPendingPeers;
    for (auto& p : pendingPeersToStop)
    {
        p->drop(ERR_MISC, "peer shutdown");
    }
    auto authenticatedPeersToStop = mAuthenticatedPeers;
    for (auto& p : authenticatedPeersToStop)
    {
        p.second->drop(ERR_MISC, "peer shutdown");
    }
}

bool
OverlayManagerImpl::isShuttingDown() const
{
    return mShuttingDown;
}
}
