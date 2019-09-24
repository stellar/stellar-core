// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/OverlayManagerImpl.h"
#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "crypto/ShortHash.h"
#include "database/Database.h"
#include "main/Application.h"
#include "main/Config.h"
#include "main/ErrorMessages.h"
#include "overlay/OverlayMetrics.h"
#include "overlay/PeerBareAddress.h"
#include "overlay/PeerManager.h"
#include "overlay/RandomPeerSource.h"
#include "overlay/TCPPeer.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/XDROperators.h"
#include "util/format.h"
#include "xdrpp/marshal.h"

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

constexpr std::chrono::seconds PEER_IP_RESOLVE_DELAY(600);

OverlayManagerImpl::PeersList::PeersList(
    OverlayManagerImpl& overlayManager,
    medida::MetricsRegistry& metricsRegistry,
    std::string const& directionString, std::string const& cancelledName,
    int maxAuthenticatedCount)
    : mConnectionsAttempted(metricsRegistry.NewMeter(
          {"overlay", directionString, "attempt"}, "connection"))
    , mConnectionsEstablished(metricsRegistry.NewMeter(
          {"overlay", directionString, "establish"}, "connection"))
    , mConnectionsDropped(metricsRegistry.NewMeter(
          {"overlay", directionString, "drop"}, "connection"))
    , mConnectionsCancelled(metricsRegistry.NewMeter(
          {"overlay", directionString, cancelledName}, "connection"))
    , mOverlayManager(overlayManager)
    , mDirectionString(directionString)
    , mMaxAuthenticatedCount(maxAuthenticatedCount)
{
}

Peer::pointer
OverlayManagerImpl::PeersList::byAddress(PeerBareAddress const& address) const
{
    auto pendingPeerIt = std::find_if(std::begin(mPending), std::end(mPending),
                                      [address](Peer::pointer const& peer) {
                                          return peer->getAddress() == address;
                                      });
    if (pendingPeerIt != std::end(mPending))
    {
        return *pendingPeerIt;
    }

    auto comp = [address](std::pair<NodeID, Peer::pointer> const& peer) {
        return peer.second->getAddress() == address;
    };

    auto authenticatedPeerIt = std::find_if(std::begin(mAuthenticated),
                                            std::end(mAuthenticated), comp);
    if (authenticatedPeerIt != std::end(mAuthenticated))
    {
        return authenticatedPeerIt->second;
    }

    auto authenticatedProbationPeerIt =
        std::find_if(std::begin(mAuthenticatedProbation),
                     std::end(mAuthenticatedProbation), comp);
    if (authenticatedProbationPeerIt != std::end(mAuthenticatedProbation))
    {
        return authenticatedProbationPeerIt->second;
    }

    return {};
}

void
OverlayManagerImpl::PeersList::removePeer(Peer* peer)
{
    CLOG(TRACE, "Overlay") << "Removing peer " << peer->toString() << " @"
                           << mOverlayManager.mApp.getConfig().PEER_PORT;
    assert(peer->getState() == Peer::CLOSING);

    auto pendingIt =
        std::find_if(std::begin(mPending), std::end(mPending),
                     [&](Peer::pointer const& p) { return p.get() == peer; });
    if (pendingIt != std::end(mPending))
    {
        CLOG(DEBUG, "Overlay") << "Dropping pending " << mDirectionString
                               << " peer: " << peer->toString();
        mPending.erase(pendingIt);
        mConnectionsDropped.Mark();
        return;
    }

    auto authentiatedIt = mAuthenticated.find(peer->getPeerID());
    if (authentiatedIt != std::end(mAuthenticated))
    {
        CLOG(DEBUG, "Overlay") << "Dropping authenticated " << mDirectionString
                               << " peer: " << peer->toString();
        mAuthenticated.erase(authentiatedIt);
        mConnectionsDropped.Mark();
        return;
    }

    auto authentiatedProbIt = mAuthenticatedProbation.find(peer->getPeerID());
    if (authentiatedProbIt != std::end(mAuthenticatedProbation))
    {
        CLOG(DEBUG, "Overlay")
            << "Dropping probation authenticated " << mDirectionString
            << " peer: " << peer->toString() << " @"
            << mOverlayManager.mApp.getConfig().PEER_PORT;
        mAuthenticatedProbation.erase(authentiatedProbIt);
        mConnectionsDropped.Mark();
        return;
    }

    CLOG(WARNING, "Overlay") << "Dropping unlisted " << mDirectionString
                             << " peer: " << peer->toString();
    CLOG(WARNING, "Overlay") << REPORT_INTERNAL_BUG;
}

Peer::pointer
OverlayManagerImpl::PeersList::lookupAuthenticated(NodeID const& id)
{
    auto it = mAuthenticated.find(id);
    if (it != mAuthenticated.end())
    {
        return it->second;
    }
    auto it2 = mAuthenticatedProbation.find(id);
    if (it2 != mAuthenticatedProbation.end())
    {
        return it2->second;
    }
    return nullptr;
}

bool
OverlayManagerImpl::PeersList::moveToAuthenticatedProbation(Peer::pointer peer)
{
    CLOG(TRACE, "Overlay") << "Moving peer " << peer->toString()
                           << " to authenticated "
                           << " state: " << peer->getState() << " @"
                           << mOverlayManager.mApp.getConfig().PEER_PORT;
    auto pendingIt = std::find(std::begin(mPending), std::end(mPending), peer);
    if (pendingIt == std::end(mPending))
    {
        CLOG(WARNING, "Overlay")
            << "Trying to move non-pending " << mDirectionString << " peer "
            << peer->toString() << " to authenticated list";
        CLOG(WARNING, "Overlay") << REPORT_INTERNAL_BUG;
        mConnectionsCancelled.Mark();
        return false;
    }

    auto p = lookupAuthenticated(peer->getPeerID());
    if (p != nullptr)
    {
        CLOG(WARNING, "Overlay")
            << "Trying to move " << mDirectionString << " peer "
            << peer->toString() << " to authenticated list again";
        CLOG(WARNING, "Overlay") << REPORT_INTERNAL_BUG;
        mConnectionsCancelled.Mark();
        return false;
    }

    mPending.erase(pendingIt);
    mAuthenticatedProbation[peer->getPeerID()] = peer;

    CLOG(DEBUG, "Overlay") << "Promoting to " << mDirectionString
                           << " probation " << peer->toString() << " @"
                           << mOverlayManager.mApp.getConfig().PEER_PORT;

    return true;
}

bool
OverlayManagerImpl::PeersList::moveToAuthenticated(Peer::pointer peer)
{
    auto pendingIt = std::find(std::begin(mPending), std::end(mPending), peer);
    if (pendingIt != std::end(mPending))
    {
        CLOG(WARNING, "Overlay")
            << "Trying to move non-probation " << mDirectionString << " peer "
            << peer->toString() << " to authenticated list"
            << " @" << mOverlayManager.mApp.getConfig().PEER_PORT;
        CLOG(WARNING, "Overlay") << REPORT_INTERNAL_BUG;
        mConnectionsCancelled.Mark();
        return false;
    }

    auto p = lookupAuthenticated(peer->getPeerID());
    if (p == nullptr)
    {
        CLOG(WARNING, "Overlay")
            << "Trying to move unknown " << mDirectionString << " peer "
            << peer->toString() << " to authenticated list"
            << " @" << mOverlayManager.mApp.getConfig().PEER_PORT;
        CLOG(WARNING, "Overlay") << REPORT_INTERNAL_BUG;
        mConnectionsCancelled.Mark();
        return false;
    }
    else if (p->getState() != Peer::GOT_AUTH_PROBATION)
    {
        CLOG(WARNING, "Overlay")
            << "Trying to move non probation " << mDirectionString << " peer "
            << peer->toString() << " to authenticated list again"
            << " @" << mOverlayManager.mApp.getConfig().PEER_PORT;
        CLOG(WARNING, "Overlay") << REPORT_INTERNAL_BUG;
        mConnectionsCancelled.Mark();
        return false;
    }

    peer->setAuthenticated();
    mAuthenticatedProbation.erase(peer->getPeerID());
    mAuthenticated[peer->getPeerID()] = peer;

    CLOG(INFO, "Overlay") << "Connected to " << peer->toString() << " @"
                          << mOverlayManager.mApp.getConfig().PEER_PORT;
    CLOG(INFO, "Overlay") << "Connected to " << peer->toString();
    CLOG(INFO, "Overlay") << "Connected to " << mDirectionString << " "
                          << peer->toString() << " @"
                          << mOverlayManager.mApp.getConfig().PEER_PORT;

    return true;
}

bool
OverlayManagerImpl::PeersList::acceptAuthenticatedPeer(Peer::pointer peer,
                                                       bool probation)
{
    CLOG(TRACE, "Overlay") << "Trying to promote peer to authenticated "
                           << peer->toString() << " @"
                           << mOverlayManager.mApp.getConfig().PEER_PORT;
    if (mOverlayManager.isPreferred(peer.get()))
    {
        if (mAuthenticated.size() < mMaxAuthenticatedCount)
        {
            return moveToAuthenticatedProbation(peer) &&
                   moveToAuthenticated(peer);
        }

        for (auto victim : mAuthenticated)
        {
            if (!mOverlayManager.isPreferred(victim.second.get()))
            {
                CLOG(INFO, "Overlay")
                    << "Evicting non-preferred " << mDirectionString << " peer "
                    << victim.second->toString() << " for preferred peer "
                    << peer->toString();
                victim.second->sendErrorAndDrop(
                    ERR_LOAD, "preferred peer selected instead",
                    Peer::DropMode::IGNORE_WRITE_QUEUE);
                return moveToAuthenticatedProbation(peer) &&
                       moveToAuthenticated(peer);
            }
        }
    }

    if (!mOverlayManager.mApp.getConfig().PREFERRED_PEERS_ONLY &&
        mAuthenticated.size() < mMaxAuthenticatedCount)
    {
        if (probation)
        {
            return moveToAuthenticatedProbation(peer);
        }
        else
        {
            return moveToAuthenticated(peer);
        }
    }
    CLOG(INFO, "Overlay") << "Non preferred " << mDirectionString
                          << " authenticated peer " << peer->toString()
                          << " rejected because all available slots are taken.";
    CLOG(INFO, "Overlay")
        << "If you wish to allow for more " << mDirectionString
        << " connections, please update your configuration file";

    if (Logging::logTrace("Overlay"))
    {
        CLOG(TRACE, "Overlay") << fmt::format(
            "limit: {}, pending: {}, probation: {}, authenticated: {}",
            mMaxAuthenticatedCount, mPending.size(),
            mAuthenticatedProbation.size(), mAuthenticated.size());
        std::stringstream pending, authprobation, authenticated;
        for (auto p : mPending)
        {
            pending << p->toString();
            pending << " ";
        }
        for (auto p : mAuthenticatedProbation)
        {
            authprobation << p.second->toString();
            authprobation << " ";
        }
        for (auto p : mAuthenticated)
        {
            authenticated << p.second->toString();
            authenticated << " ";
        }
        CLOG(TRACE, "Overlay") << fmt::format(
            "pending: [{}] probation: [{}] authenticated: [{}]", pending.str(),
            authprobation.str(), authenticated.str());
    }

    mConnectionsCancelled.Mark();
    return false;
}

void
OverlayManagerImpl::PeersList::shutdown()
{
    auto pendingPeersToStop = mPending;
    for (auto& p : pendingPeersToStop)
    {
        p->sendErrorAndDrop(ERR_MISC, "shutdown",
                            Peer::DropMode::IGNORE_WRITE_QUEUE);
    }
    auto authenticatedPeersToStop = mAuthenticated;
    for (auto& p : authenticatedPeersToStop)
    {
        p.second->sendErrorAndDrop(ERR_MISC, "shutdown",
                                   Peer::DropMode::IGNORE_WRITE_QUEUE);
    }
}

std::unique_ptr<OverlayManager>
OverlayManager::create(Application& app)
{
    return std::make_unique<OverlayManagerImpl>(app);
}

OverlayManagerImpl::OverlayManagerImpl(Application& app)
    : mApp(app)
    , mInboundPeers(*this, mApp.getMetrics(), "inbound", "reject",
                    mApp.getConfig().MAX_ADDITIONAL_PEER_CONNECTIONS)
    , mOutboundPeers(*this, mApp.getMetrics(), "outbound", "cancel",
                     mApp.getConfig().TARGET_PEER_CONNECTIONS)
    , mPeerManager(app)
    , mDoor(mApp)
    , mAuth(mApp)
    , mShuttingDown(false)
    , mOverlayMetrics(app)
    , mMessageCache(0xffff)
    , mCheckPerfLogLevelCounter(0)
    , mPerfLogLevel(Logging::getLogLevel("Perf"))
    , mTimer(app)
    , mPeerIPTimer(app)
    , mFloodGate(app)
{
    mPeerSources[PeerType::INBOUND] = std::make_unique<RandomPeerSource>(
        mPeerManager, RandomPeerSource::nextAttemptCutoff(PeerType::INBOUND));
    mPeerSources[PeerType::OUTBOUND] = std::make_unique<RandomPeerSource>(
        mPeerManager, RandomPeerSource::nextAttemptCutoff(PeerType::OUTBOUND));
    mPeerSources[PeerType::PREFERRED] = std::make_unique<RandomPeerSource>(
        mPeerManager, RandomPeerSource::nextAttemptCutoff(PeerType::PREFERRED));
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
                purgeDeadPeers();
                triggerPeerResolution();
                tick();
            },
            VirtualTimer::onFailureNoop);
    }
}

void
OverlayManagerImpl::connectTo(PeerBareAddress const& address)
{
    connectToImpl(address, false);
}

bool
OverlayManagerImpl::connectToImpl(PeerBareAddress const& address,
                                  bool forceoutbound)
{
    CLOG(TRACE, "Overlay") << "Connect to " << address.toString() << " @"
                           << mApp.getConfig().PEER_PORT;
    CLOG(TRACE, "Overlay") << fmt::format(
        "Attempting to connect to {} force={} @{}", address.toString(),
        forceoutbound, mApp.getConfig().PEER_PORT);
    auto currentConnection = getConnectedPeer(address);
    if (!currentConnection || (forceoutbound && currentConnection->getRole() ==
                                                    Peer::REMOTE_CALLED_US))
    {
        getPeerManager().update(address, PeerManager::BackOffUpdate::INCREASE);
        return addOutboundConnection(TCPPeer::initiate(mApp, address));
    }
    else
    {
        CLOG(ERROR, "Overlay")
            << "trying to connect to a node we're already connected to "
            << address.toString();
        CLOG(ERROR, "Overlay") << REPORT_INTERNAL_BUG;
        return false;
    }
}

OverlayManagerImpl::PeersList&
OverlayManagerImpl::getPeersList(Peer* peer)
{
    switch (peer->getRole())
    {
    case Peer::WE_CALLED_REMOTE:
        return mOutboundPeers;
    case Peer::REMOTE_CALLED_US:
        return mInboundPeers;
    default:
        abort();
    }
}

void
OverlayManagerImpl::storePeerList(std::vector<PeerBareAddress> const& addresses,
                                  bool setPreferred, bool startup)
{
    auto typeUpgrade = setPreferred
                           ? PeerManager::TypeUpdate::SET_PREFERRED
                           : PeerManager::TypeUpdate::UPDATE_TO_OUTBOUND;
    if (setPreferred)
    {
        mConfigurationPreferredPeers.clear();
    }

    for (auto const& peer : addresses)
    {
        if (setPreferred)
        {
            mConfigurationPreferredPeers.insert(peer);
        }

        if (startup)
        {
            getPeerManager().update(peer, typeUpgrade,
                                    PeerManager::BackOffUpdate::HARD_RESET);
        }
        else
        {
            // If address is present in the DB, `update` will ensure
            // type is correctly updated. Otherwise, a new entry is created.
            // Note that this won't downgrade preferred peers back to outbound.
            getPeerManager().update(peer, typeUpgrade);
        }
    }
}

void
OverlayManagerImpl::storeConfigPeers()
{
    // Synchronously resolve and store peers from the config
    storePeerList(resolvePeers(mApp.getConfig().KNOWN_PEERS), false, true);
    storePeerList(resolvePeers(mApp.getConfig().PREFERRED_PEERS), true, true);
}

void
OverlayManagerImpl::purgeDeadPeers()
{
    getPeerManager().removePeersWithManyFailures(
        Config::REALLY_DEAD_NUM_FAILURES_CUTOFF);
}

void
OverlayManagerImpl::triggerPeerResolution()
{
    assert(!mResolvedPeers.valid());

    // Trigger DNS resolution on the background thread
    using task_t = std::packaged_task<ResolvedPeers()>;
    std::shared_ptr<task_t> task = std::make_shared<task_t>([this]() {
        if (!this->mShuttingDown)
        {
            auto known = resolvePeers(this->mApp.getConfig().KNOWN_PEERS);
            auto preferred =
                resolvePeers(this->mApp.getConfig().PREFERRED_PEERS);
            return ResolvedPeers{known, preferred};
        }
        return ResolvedPeers{};
    });

    mResolvedPeers = task->get_future();
    mApp.postOnBackgroundThread(bind(&task_t::operator(), task),
                                "OverlayManager: resolve peer IPs");
}

std::vector<PeerBareAddress>
OverlayManagerImpl::resolvePeers(std::vector<string> const& peers)
{
    std::vector<PeerBareAddress> addresses;
    for (auto const& peer : peers)
    {
        try
        {
            addresses.push_back(PeerBareAddress::resolve(peer, mApp));
        }
        catch (std::runtime_error& e)
        {
            CLOG(ERROR, "Overlay")
                << "Unable to resolve peer '" << peer << "': " << e.what();
            CLOG(ERROR, "Overlay") << "Peer may be no longer available under "
                                      "this address. Please update your "
                                      "PREFERRED_PEERS and KNOWN_PEERS "
                                      "settings in configuration file";
        }
    }
    return addresses;
}

std::vector<PeerBareAddress>
OverlayManagerImpl::getPeersToConnectTo(int maxNum, PeerType peerType)
{
    assert(maxNum >= 0);
    if (maxNum == 0)
    {
        return {};
    }

    int ignored = 0;
    auto keep = [&](PeerBareAddress const& address) {
        auto outPeer = mOutboundPeers.byAddress(address);
        if (outPeer)
        {
            // we've already got an outbound connection
            ignored++;
            return false;
        }
        else
        {
            auto inPeer = mInboundPeers.byAddress(address);

            auto promote = inPeer && (peerType == PeerType::INBOUND) &&
                           (inPeer->getRole() == Peer::REMOTE_CALLED_US);
            // if the other connection is not yet fully authenticated, we
            // initiate this one as well
            auto res = !inPeer || /* !inPeer->isAuthenticated() || */ promote;
            if (!res)
            {
                ignored++;
            }
            return res;
        }
    };

    // don't connect to too many peers at once
    auto res =
        mPeerSources[peerType]->getRandomPeers(std::min(maxNum, 50), keep);
    if (Logging::logDebug("Overlay"))
    {
        std::stringstream s;
        for (auto& p : res)
        {
            s << p.toString() << " ";
        }
        CLOG(DEBUG, "Overlay") << fmt::format(
            "peers to connect to t={}, max={}, skipped={}: [{}]",
            static_cast<int>(peerType), maxNum, ignored, s.str());
    }
    return res;
}

int
OverlayManagerImpl::connectTo(int maxNum, PeerType peerType)
{
    return connectTo(getPeersToConnectTo(maxNum, peerType),
                     peerType == PeerType::INBOUND);
}

int
OverlayManagerImpl::connectTo(std::vector<PeerBareAddress> const& peers,
                              bool forceoutbound)
{
    auto count = 0;
    for (auto& address : peers)
    {
        if (connectToImpl(address, forceoutbound))
        {
            count++;
        }
    }
    return count;
}

void
OverlayManagerImpl::peerMaintenance()
{
    // first, check if we take peers out of probation
    for (auto const& pp : getAuthenticatedProbationPeers())
    {
        auto& p = pp.second;
        CLOG(TRACE, "Overlay") << "Peer age " << p->toString() << "/"
                               << static_cast<int>(p->getRole()) << " : "
                               << p->getLifeTime().count();
        if (p->isPassedProbation())
        {
            if (!acceptAuthenticatedPeer(p, false))
            {
                p->sendErrorAndDrop(
                    ERR_MISC, "Could not take peer out of probation state",
                    Peer::DropMode::FLUSH_WRITE_QUEUE);
            }
        }
    }
    // now, see if some peers in probation should be disconnected
    auto authenticated = getAuthenticatedPeers();
    for (auto const& pp : getAuthenticatedProbationPeers())
    {
        auto& p = pp.second;
        if (authenticated.find(p->getPeerID()) != authenticated.end())
        {
            p->sendErrorAndDrop(ERR_MISC, "Peer already authenticated",
                                Peer::DropMode::FLUSH_WRITE_QUEUE);
        }
    }
}

// called every 2 seconds
void
OverlayManagerImpl::tick()
{
    CLOG(TRACE, "Overlay") << "OverlayManagerImpl tick  @"
                           << mApp.getConfig().PEER_PORT;

    mLoad.maybeShedExcessLoad(mApp);

    if (mResolvedPeers.valid() &&
        mResolvedPeers.wait_for(std::chrono::nanoseconds(1)) ==
            std::future_status::ready)
    {
        CLOG(TRACE, "Overlay") << "Resolved peers are ready";
        auto res = mResolvedPeers.get();
        storePeerList(res.known, false, false);
        storePeerList(res.preferred, true, false);
        mPeerIPTimer.expires_from_now(PEER_IP_RESOLVE_DELAY);
        mPeerIPTimer.async_wait([this]() { this->triggerPeerResolution(); },
                                VirtualTimer::onFailureNoop);
    }

    peerMaintenance();

    auto availablePendingSlots = availableOutboundPendingSlots();
    auto availableAuthenticatedSlots = availableOutboundAuthenticatedSlots();
    auto nonPreferredCount = nonPreferredAuthenticatedCount();

    // if all of our outbound peers are preferred, we don't need to try any
    // longer
    if (availableAuthenticatedSlots > 0 || nonPreferredCount > 0)
    {
        // try to replace all connections with preferred peers
        auto pendingUsedByPreferred = connectTo(
            mApp.getConfig().TARGET_PEER_CONNECTIONS, PeerType::PREFERRED);

        assert(pendingUsedByPreferred <= availablePendingSlots);
        availablePendingSlots -= pendingUsedByPreferred;

        // connect to non-preferred candidates from the database when
        // PREFERRED_PEER_ONLY is set and we connect to a non preferred_peer we
        // just end up dropping & backing off it during handshake (this allows
        // for preferred_peers to work for both ip based and key based preferred
        // mode)
        if (availablePendingSlots > 0 && availableAuthenticatedSlots > 0)
        {
            // try to leave at least some pending slots for peer promotion
            constexpr const auto RESERVED_FOR_PROMOTION = 1;
            auto outboundToConnect =
                availablePendingSlots > RESERVED_FOR_PROMOTION
                    ? std::min(availablePendingSlots - RESERVED_FOR_PROMOTION,
                               availableAuthenticatedSlots)
                    : RESERVED_FOR_PROMOTION;
            auto pendingUsedByOutbound =
                connectTo(outboundToConnect, PeerType::OUTBOUND);
            assert(pendingUsedByOutbound <= availablePendingSlots);
            availablePendingSlots -= pendingUsedByOutbound;
        }
    }

    // try to promote some peers from inbound to outbound state
    if (availablePendingSlots > 0)
    {
        connectTo(availablePendingSlots, PeerType::INBOUND);
    }

    mTimer.expires_from_now(
        std::chrono::seconds(mApp.getConfig().PEER_AUTHENTICATION_TIMEOUT + 1));
    mTimer.async_wait([this]() { this->tick(); }, VirtualTimer::onFailureNoop);
}

int
OverlayManagerImpl::availableOutboundPendingSlots() const
{
    if (mOutboundPeers.mPending.size() <
        mApp.getConfig().MAX_OUTBOUND_PENDING_CONNECTIONS)
    {
        return static_cast<int>(
            mApp.getConfig().MAX_OUTBOUND_PENDING_CONNECTIONS -
            mOutboundPeers.mPending.size() +
            mOutboundPeers.mAuthenticatedProbation.size());
    }
    else
    {
        return 0;
    }
}

int
OverlayManagerImpl::availableOutboundAuthenticatedSlots() const
{
    if (mOutboundPeers.mAuthenticated.size() <
        mApp.getConfig().TARGET_PEER_CONNECTIONS)
    {
        return static_cast<int>(mApp.getConfig().TARGET_PEER_CONNECTIONS -
                                mOutboundPeers.mAuthenticated.size());
    }
    else
    {
        return 0;
    }
}

int
OverlayManagerImpl::nonPreferredAuthenticatedCount() const
{
    unsigned short nonPreferredCount{0};
    for (auto const& p : mOutboundPeers.mAuthenticated)
    {
        if (!isPreferred(p.second.get()))
        {
            nonPreferredCount++;
        }
    }

    assert(nonPreferredCount <= mApp.getConfig().TARGET_PEER_CONNECTIONS);
    return nonPreferredCount;
}

Peer::pointer
OverlayManagerImpl::getConnectedPeer(PeerBareAddress const& address)
{
    // the order here is important:
    // callers use this to decide if they should attempt outbound connections
    // so we want to guarantee that outbound connections are never duplicated
    auto outbound = mOutboundPeers.byAddress(address);
    return outbound ? outbound : mInboundPeers.byAddress(address);
}

void
OverlayManagerImpl::ledgerClosed(uint32_t lastClosedledgerSeq)
{
    mFloodGate.clearBelow(lastClosedledgerSeq);
}

void
OverlayManagerImpl::updateSizeCounters()
{
    mOverlayMetrics.mPendingPeersSize.set_count(getPendingPeersCount());
    mOverlayMetrics.mAuthenticatedPeersSize.set_count(
        getAuthenticatedPeersCount());
}

void
OverlayManagerImpl::addInboundConnection(Peer::pointer peer)
{
    assert(peer->getRole() == Peer::REMOTE_CALLED_US);
    mInboundPeers.mConnectionsAttempted.Mark();

    size_t pendingInbound = mInboundPeers.mPending.size() +
                            mInboundPeers.mAuthenticatedProbation.size();
    auto haveSpace =
        pendingInbound < mApp.getConfig().MAX_INBOUND_PENDING_CONNECTIONS;
    if (!haveSpace &&
        pendingInbound < mApp.getConfig().MAX_INBOUND_PENDING_CONNECTIONS +
                             Config::POSSIBLY_PREFERRED_EXTRA)
    {
        // for peers that are possibly preferred (they have the same IP as some
        // preferred peer we enocuntered in past), we allow an extra
        // Config::POSSIBLY_PREFERRED_EXTRA incoming pending connections, that
        // are not available for non-preferred peers
        haveSpace = isPossiblyPreferred(peer->getIP());
    }

    if (mShuttingDown || !haveSpace)
    {
        if (!mShuttingDown)
        {
            CLOG(DEBUG, "Overlay")
                << "Peer rejected - all pending inbound connections are taken: "
                << peer->toString();
            CLOG(DEBUG, "Overlay") << "If you wish to allow for more pending "
                                      "inbound connections, please update your "
                                      "MAX_PENDING_CONNECTIONS setting in "
                                      "configuration file.";
        }

        mInboundPeers.mConnectionsCancelled.Mark();
        peer->drop("all pending inbound connections are taken",
                   Peer::DropDirection::WE_DROPPED_REMOTE,
                   Peer::DropMode::IGNORE_WRITE_QUEUE);
        return;
    }
    CLOG(DEBUG, "Overlay") << "New connected peer " << peer->toString() << " @"
                           << mApp.getConfig().PEER_PORT;
    mInboundPeers.mConnectionsEstablished.Mark();
    mInboundPeers.mPending.push_back(peer);
    updateSizeCounters();
}

bool
OverlayManagerImpl::isPossiblyPreferred(std::string const& ip)
{
    return std::any_of(
        std::begin(mConfigurationPreferredPeers),
        std::end(mConfigurationPreferredPeers),
        [&](PeerBareAddress const& address) { return address.getIP() == ip; });
}

bool
OverlayManagerImpl::addOutboundConnection(Peer::pointer peer)
{
    assert(peer->getRole() == Peer::WE_CALLED_REMOTE);
    mOutboundPeers.mConnectionsAttempted.Mark();

    if (mShuttingDown || mOutboundPeers.mPending.size() >=
                             mApp.getConfig().MAX_OUTBOUND_PENDING_CONNECTIONS)
    {
        if (!mShuttingDown)
        {
            CLOG(DEBUG, "Overlay")
                << "Peer rejected - all outbound connections taken: "
                << peer->toString() << " @" << mApp.getConfig().PEER_PORT;
            CLOG(DEBUG, "Overlay") << "If you wish to allow for more pending "
                                      "outbound connections, please update "
                                      "your MAX_PENDING_CONNECTIONS setting in "
                                      "configuration file.";
        }

        mOutboundPeers.mConnectionsCancelled.Mark();
        peer->drop("all outbound connections taken",
                   Peer::DropDirection::WE_DROPPED_REMOTE,
                   Peer::DropMode::IGNORE_WRITE_QUEUE);
        return false;
    }
    CLOG(DEBUG, "Overlay") << "New connected peer " << peer->toString() << " @"
                           << mApp.getConfig().PEER_PORT;
    mOutboundPeers.mConnectionsEstablished.Mark();
    mOutboundPeers.mPending.push_back(peer);
    updateSizeCounters();

    return true;
}

void
OverlayManagerImpl::removePeer(Peer* peer)
{
    getPeersList(peer).removePeer(peer);
    getPeerManager().removePeersWithManyFailures(
        Config::REALLY_DEAD_NUM_FAILURES_CUTOFF, &peer->getAddress());
    updateSizeCounters();
}

bool
OverlayManagerImpl::moveToAuthenticated(Peer::pointer peer)
{
    auto result = getPeersList(peer.get()).moveToAuthenticated(peer);
    updateSizeCounters();
    return result;
}

bool
OverlayManagerImpl::acceptAuthenticatedPeer(Peer::pointer peer, bool probation)
{
    return getPeersList(peer.get()).acceptAuthenticatedPeer(peer, probation);
}

std::vector<Peer::pointer> const&
OverlayManagerImpl::getInboundPendingPeers() const
{
    return mInboundPeers.mPending;
}

std::vector<Peer::pointer> const&
OverlayManagerImpl::getOutboundPendingPeers() const
{
    return mOutboundPeers.mPending;
}

std::vector<Peer::pointer>
OverlayManagerImpl::getPendingPeers() const
{
    auto result = mOutboundPeers.mPending;
    result.insert(std::end(result), std::begin(mInboundPeers.mPending),
                  std::end(mInboundPeers.mPending));
    return result;
}

std::map<NodeID, Peer::pointer>
OverlayManagerImpl::getInboundAnyAuthenticatedPeers() const
{
    auto result = mInboundPeers.mAuthenticated;
    result.insert(std::begin(mInboundPeers.mAuthenticatedProbation),
                  std::end(mInboundPeers.mAuthenticatedProbation));
    return result;
}

std::map<NodeID, Peer::pointer>
OverlayManagerImpl::getOutboundAnyAuthenticatedPeers() const
{
    auto result = mOutboundPeers.mAuthenticated;
    result.insert(std::begin(mOutboundPeers.mAuthenticatedProbation),
                  std::end(mOutboundPeers.mAuthenticatedProbation));

    return result;
}

std::multimap<NodeID, Peer::pointer>
OverlayManagerImpl::getAnyAuthenticatedPeers() const
{
    std::multimap<NodeID, Peer::pointer> res;

    auto i = getInboundAnyAuthenticatedPeers();
    res.insert(i.begin(), i.end());
    auto o = getOutboundAnyAuthenticatedPeers();
    res.insert(o.begin(), o.end());

    return res;
}

std::map<NodeID, Peer::pointer>
OverlayManagerImpl::getAuthenticatedPeers() const
{
    auto result = mOutboundPeers.mAuthenticated;
    result.insert(std::begin(mInboundPeers.mAuthenticated),
                  std::end(mInboundPeers.mAuthenticated));
    return result;
}

std::multimap<NodeID, Peer::pointer>
OverlayManagerImpl::getAuthenticatedProbationPeers() const
{
    std::multimap<NodeID, Peer::pointer> result;
    result.insert(std::begin(mOutboundPeers.mAuthenticatedProbation),
                  std::end(mOutboundPeers.mAuthenticatedProbation));
    result.insert(std::begin(mInboundPeers.mAuthenticatedProbation),
                  std::end(mInboundPeers.mAuthenticatedProbation));
    return result;
}

int
OverlayManagerImpl::getPendingPeersCount() const
{
    size_t s = mInboundPeers.mPending.size() + mOutboundPeers.mPending.size();
    s += mInboundPeers.mAuthenticatedProbation.size() +
         mOutboundPeers.mAuthenticatedProbation.size();
    return static_cast<int>(s);
}

int
OverlayManagerImpl::getAuthenticatedPeersCount() const
{
    return static_cast<int>(mInboundPeers.mAuthenticated.size() +
                            mOutboundPeers.mAuthenticated.size());
}

bool
OverlayManagerImpl::isPreferred(Peer* peer) const
{
    std::string pstr = peer->toString();

    if (mConfigurationPreferredPeers.find(peer->getAddress()) !=
        mConfigurationPreferredPeers.end())
    {
        CLOG(DEBUG, "Overlay") << "Peer " << pstr << " is preferred  @"
                               << mApp.getConfig().PEER_PORT;
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
                << "Peer key "
                << mApp.getConfig().toShortString(peer->getPeerID())
                << " is preferred @" << mApp.getConfig().PEER_PORT;
            return true;
        }
    }

    CLOG(DEBUG, "Overlay") << "Peer " << pstr << " is not preferred @"
                           << mApp.getConfig().PEER_PORT;
    return false;
}

std::vector<Peer::pointer>
OverlayManagerImpl::getRandomAuthenticatedPeers()
{
    auto goodPeers = std::vector<Peer::pointer>{};
    auto extractPeer = [](std::pair<NodeID, Peer::pointer> const& peer) {
        return peer.second;
    };
    std::transform(std::begin(mInboundPeers.mAuthenticated),
                   std::end(mInboundPeers.mAuthenticated),
                   std::back_inserter(goodPeers), extractPeer);
    std::transform(std::begin(mOutboundPeers.mAuthenticated),
                   std::end(mOutboundPeers.mAuthenticated),
                   std::back_inserter(goodPeers), extractPeer);
    std::transform(std::begin(mOutboundPeers.mAuthenticatedProbation),
                   std::end(mOutboundPeers.mAuthenticatedProbation),
                   std::back_inserter(goodPeers), extractPeer);
    std::shuffle(goodPeers.begin(), goodPeers.end(), gRandomEngine);
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
    mOverlayMetrics.mMessagesBroadcast.Mark();
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

OverlayMetrics&
OverlayManagerImpl::getOverlayMetrics()
{
    return mOverlayMetrics;
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
    mInboundPeers.shutdown();
    mOutboundPeers.shutdown();

    // Stop ticking and resolving peers
    mTimer.cancel();
    mPeerIPTimer.cancel();
}

bool
OverlayManagerImpl::isShuttingDown() const
{
    return mShuttingDown;
}

static void
logDuplicateMessage(el::Level level, size_t size, std::string const& dupOrUniq,
                    std::string const& fetchOrFlood)
{
    if (level == el::Level::Trace)
    {
        CLOG(TRACE, "Perf") << "Received " << size << " " << dupOrUniq
                            << " bytes of " << fetchOrFlood << " message";
    }
}

void
OverlayManagerImpl::recordDuplicateMessageMetric(
    StellarMessage const& stellarMsg)
{
    bool flood = false;
    if (stellarMsg.type() == TRANSACTION || stellarMsg.type() == SCP_MESSAGE)
    {
        flood = true;
    }
    else if (stellarMsg.type() != TX_SET && stellarMsg.type() != SCP_QUORUMSET)
    {
        return;
    }

    if (++mCheckPerfLogLevelCounter >= 100)
    {
        mCheckPerfLogLevelCounter = 0;
        mPerfLogLevel = Logging::getLogLevel("Perf");
    }

    size_t size = xdr::xdr_argpack_size(stellarMsg);
    auto hash = shortHash::xdrComputeHash(stellarMsg);
    if (mMessageCache.exists(hash))
    {
        if (flood)
        {
            mOverlayMetrics.mDuplicateFloodBytesRecv.Mark(size);
            logDuplicateMessage(mPerfLogLevel, size, "duplicate", "flood");
        }
        else
        {
            mOverlayMetrics.mDuplicateFetchBytesRecv.Mark(size);
            logDuplicateMessage(mPerfLogLevel, size, "duplicate", "fetch");
        }
    }
    else
    {
        // NOTE: false is used here as a placeholder value, since no value is
        // needed.
        mMessageCache.put(hash, false);
        if (flood)
        {
            mOverlayMetrics.mUniqueFloodBytesRecv.Mark(size);
            logDuplicateMessage(mPerfLogLevel, size, "unique", "flood");
        }
        else
        {
            mOverlayMetrics.mUniqueFetchBytesRecv.Mark(size);
            logDuplicateMessage(mPerfLogLevel, size, "unique", "fetch");
        }
    }
}
}
