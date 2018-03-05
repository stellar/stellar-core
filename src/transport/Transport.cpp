// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transport/Transport.h"
#include "main/Application.h"
#include "transport/PreferredPeers.h"
#include "transport/TCPPeer.h"
#include "util/Logging.h"

#include <medida/counter.h>
#include <medida/meter.h>
#include <medida/metrics_registry.h>

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

using xdr::operator<;

Transport::Transport(Application& app)
    : mApp(app)
    , mShuttingDown(false)
    , mConnectionsAttempted(mApp.getMetrics().NewMeter(
          {"overlay", "connection", "attempt"}, "connection"))
    , mConnectionsEstablished(mApp.getMetrics().NewMeter(
          {"overlay", "connection", "establish"}, "connection"))
    , mConnectionsDropped(mApp.getMetrics().NewMeter(
          {"overlay", "connection", "drop"}, "connection"))
    , mConnectionsRejected(mApp.getMetrics().NewMeter(
          {"overlay", "connection", "reject"}, "connection"))
    , mPendingPeersSize(
          mApp.getMetrics().NewCounter({"overlay", "memory", "pending-peers"}))
    , mAuthenticatedPeersSize(mApp.getMetrics().NewCounter(
          {"overlay", "memory", "authenticated-peers"}))
{
}

Transport::~Transport()
{
}

void
Transport::connectTo(std::string const& peerStr)
{
    try
    {
        auto address = PeerBareAddress::resolve(peerStr, mApp);
        connectTo(address);
    }
    catch (const std::runtime_error&)
    {
        CLOG(ERROR, "Overlay") << "Unable to add peer '" << peerStr << "'";
    }
}

void
Transport::connectTo(PeerBareAddress const& address)
{
    auto pr = PeerRecord{address, mApp.getClock().now(), 0};
    connectTo(pr);
}

void
Transport::connectTo(PeerRecord& pr)
{
    mConnectionsAttempted.Mark();
    if (!getConnectedPeer(pr.getAddress()))
    {
        pr.backOff(mApp.getClock());
        pr.storePeerRecord(mApp.getDatabase());

        if (getPendingPeersCount() < mApp.getConfig().MAX_PENDING_CONNECTIONS)
        {
            addPendingPeer(TCPPeer::initiate(mApp, pr.getAddress()));
        }
        else
        {
            CLOG(DEBUG, "Overlay")
                << "reached maximum number of pending connections, backing off "
                << pr.toString();
        }
    }
    else
    {
        CLOG(ERROR, "Overlay")
            << "trying to connect to a node we're already connected to "
            << pr.toString();
    }
}

Peer::pointer
Transport::getConnectedPeer(PeerBareAddress const& address)
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
Transport::updateSizeCounters()
{
    mPendingPeersSize.set_count(getPendingPeersCount());
    mAuthenticatedPeersSize.set_count(getAuthenticatedPeersCount());
}

void
Transport::addPendingPeer(Peer::pointer peer)
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
Transport::dropPeer(Peer* peer)
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
Transport::acceptAuthenticatedPeer(Peer::pointer peer)
{
    if (mApp.getPreferredPeers().isPreferred(peer.get()))
    {
        if (getAuthenticatedPeersCount() <
            mApp.getConfig().MAX_PEER_CONNECTIONS)
        {
            return moveToAuthenticated(peer);
        }

        auto authenticatedPeers = getAuthenticatedPeers();
        for (auto victim : authenticatedPeers)
        {
            if (!mApp.getPreferredPeers().isPreferred(victim.second.get()))
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

bool
Transport::moveToAuthenticated(Peer::pointer peer)
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
    noteHandshakeSuccessInPeerRecord(peer);
    return true;
}

std::vector<Peer::pointer> const&
Transport::getPendingPeers() const
{
    return mPendingPeers;
}

std::map<NodeID, Peer::pointer> const&
Transport::getAuthenticatedPeers() const
{
    return mAuthenticatedPeers;
}

int
Transport::getPendingPeersCount() const
{
    return static_cast<int>(mPendingPeers.size());
}

int
Transport::getAuthenticatedPeersCount() const
{
    return static_cast<int>(mAuthenticatedPeers.size());
}

std::vector<Peer::pointer>
Transport::getRandomAuthenticatedPeers()
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
Transport::shutdown()
{
    if (mShuttingDown)
    {
        return;
    }
    mShuttingDown = true;
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
Transport::isShuttingDown() const
{
    return mShuttingDown;
}

void
Transport::noteHandshakeSuccessInPeerRecord(Peer::pointer peer)
{
    auto address = peer->getAddress();
    assert(!address.isEmpty());
    auto pr = PeerRecord::loadPeerRecord(mApp.getDatabase(), address);
    if (pr)
    {
        pr->resetBackOff(mApp.getClock());
    }
    else
    {
        pr = make_optional<PeerRecord>(address, mApp.getClock().now());
    }
    CLOG(INFO, "Overlay") << "successful handshake with "
                          << mApp.getConfig().toShortString(peer->getPeerID())
                          << "@" << pr->toString();
    pr->setPreferred(mApp.getPreferredPeers().isPreferred(peer.get()));
    pr->storePeerRecord(mApp.getDatabase());
}
}
