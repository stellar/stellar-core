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
#include "util/Thread.h"
#include "util/XDROperators.h"
#include "xdrpp/marshal.h"
#include <Tracy.hpp>
#include <fmt/format.h>

#include "medida/counter.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"

#include <algorithm>
#include <random>

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
    ZoneScoped;
    auto pendingPeerIt = std::find_if(std::begin(mPending), std::end(mPending),
                                      [address](Peer::pointer const& peer) {
                                          return peer->getAddress() == address;
                                      });
    if (pendingPeerIt != std::end(mPending))
    {
        return *pendingPeerIt;
    }

    auto authenticatedPeerIt =
        std::find_if(std::begin(mAuthenticated), std::end(mAuthenticated),
                     [address](std::pair<NodeID, Peer::pointer> const& peer) {
                         return peer.second->getAddress() == address;
                     });
    if (authenticatedPeerIt != std::end(mAuthenticated))
    {
        return authenticatedPeerIt->second;
    }

    return {};
}

void
OverlayManagerImpl::PeersList::removePeer(Peer* peer)
{
    ZoneScoped;
    CLOG_TRACE(Overlay, "Removing peer {} @{}", peer->toString(),
               mOverlayManager.mApp.getConfig().PEER_PORT);
    assert(peer->getState() == Peer::CLOSING);

    auto pendingIt =
        std::find_if(std::begin(mPending), std::end(mPending),
                     [&](Peer::pointer const& p) { return p.get() == peer; });
    if (pendingIt != std::end(mPending))
    {
        CLOG_TRACE(Overlay, "Dropping pending {} peer: {}", mDirectionString,
                   peer->toString());
        mPending.erase(pendingIt);
        mConnectionsDropped.Mark();
        return;
    }

    auto authentiatedIt = mAuthenticated.find(peer->getPeerID());
    if (authentiatedIt != std::end(mAuthenticated))
    {
        CLOG_DEBUG(Overlay, "Dropping authenticated {} peer: {}",
                   mDirectionString, peer->toString());
        mAuthenticated.erase(authentiatedIt);
        mConnectionsDropped.Mark();
        return;
    }

    CLOG_WARNING(Overlay, "Dropping unlisted {} peer: {}", mDirectionString,
                 peer->toString());
    CLOG_WARNING(Overlay, "{}", REPORT_INTERNAL_BUG);
}

bool
OverlayManagerImpl::PeersList::moveToAuthenticated(Peer::pointer peer)
{
    ZoneScoped;
    CLOG_TRACE(Overlay, "Moving peer {} to authenticated  state: {} @{}",
               peer->toString(), peer->getState(),
               mOverlayManager.mApp.getConfig().PEER_PORT);
    auto pendingIt = std::find(std::begin(mPending), std::end(mPending), peer);
    if (pendingIt == std::end(mPending))
    {
        CLOG_WARNING(
            Overlay,
            "Trying to move non-pending {} peer {} to authenticated list",
            mDirectionString, peer->toString());
        CLOG_WARNING(Overlay, "{}", REPORT_INTERNAL_BUG);
        mConnectionsCancelled.Mark();
        return false;
    }

    auto authenticatedIt = mAuthenticated.find(peer->getPeerID());
    if (authenticatedIt != std::end(mAuthenticated))
    {
        CLOG_WARNING(Overlay,
                     "Trying to move authenticated {} peer {} to authenticated "
                     "list again",
                     mDirectionString, peer->toString());
        CLOG_WARNING(Overlay, "{}", REPORT_INTERNAL_BUG);
        mConnectionsCancelled.Mark();
        return false;
    }

    mPending.erase(pendingIt);
    mAuthenticated[peer->getPeerID()] = peer;

    CLOG_INFO(Overlay, "Connected to {}", peer->toString());

    return true;
}

bool
OverlayManagerImpl::PeersList::acceptAuthenticatedPeer(Peer::pointer peer)
{
    ZoneScoped;
    CLOG_TRACE(Overlay, "Trying to promote peer to authenticated {} @{}",
               peer->toString(), mOverlayManager.mApp.getConfig().PEER_PORT);
    if (mOverlayManager.isPreferred(peer.get()))
    {
        if (mAuthenticated.size() < mMaxAuthenticatedCount)
        {
            return moveToAuthenticated(peer);
        }

        for (auto victim : mAuthenticated)
        {
            if (!mOverlayManager.isPreferred(victim.second.get()))
            {
                CLOG_INFO(
                    Overlay,
                    "Evicting non-preferred {} peer {} for preferred peer {}",
                    mDirectionString, victim.second->toString(),
                    peer->toString());
                victim.second->sendErrorAndDrop(
                    ERR_LOAD, "preferred peer selected instead",
                    Peer::DropMode::IGNORE_WRITE_QUEUE);
                return moveToAuthenticated(peer);
            }
        }
    }

    if (!mOverlayManager.mApp.getConfig().PREFERRED_PEERS_ONLY &&
        mAuthenticated.size() < mMaxAuthenticatedCount)
    {
        return moveToAuthenticated(peer);
    }

    CLOG_INFO(Overlay,
              "Non preferred {} authenticated peer {} rejected because all "
              "available slots are taken.",
              mDirectionString, peer->toString());
    CLOG_INFO(
        Overlay,
        "If you wish to allow for more {} connections, please update your "
        "configuration file",
        mDirectionString);

    if (Logging::logTrace("Overlay"))
    {
        CLOG_TRACE(Overlay, "limit: {}, pending: {}, authenticated: {}",
                   mMaxAuthenticatedCount, mPending.size(),
                   mAuthenticated.size());
        std::stringstream pending, authenticated;
        for (auto p : mPending)
        {
            pending << p->toString();
            pending << " ";
        }
        for (auto p : mAuthenticated)
        {
            authenticated << p.second->toString();
            authenticated << " ";
        }
        CLOG_TRACE(Overlay, "pending: [{}] authenticated: [{}]", pending.str(),
                   authenticated.str());
    }

    mConnectionsCancelled.Mark();
    return false;
}

void
OverlayManagerImpl::PeersList::shutdown()
{
    ZoneScoped;
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
    , mTimer(app)
    , mPeerIPTimer(app)
    , mFloodGate(app)
    , mSurveyManager(make_shared<SurveyManager>(app))
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
    ZoneScoped;
    connectToImpl(address, false);
}

bool
OverlayManagerImpl::connectToImpl(PeerBareAddress const& address,
                                  bool forceoutbound)
{
    CLOG_TRACE(Overlay, "Connect to {} @{}", address.toString(),
               mApp.getConfig().PEER_PORT);
    auto currentConnection = getConnectedPeer(address);
    if (!currentConnection || (forceoutbound && currentConnection->getRole() ==
                                                    Peer::REMOTE_CALLED_US))
    {
        getPeerManager().update(address, PeerManager::BackOffUpdate::INCREASE);
        return addOutboundConnection(TCPPeer::initiate(mApp, address));
    }
    else
    {
        CLOG_ERROR(Overlay,
                   "trying to connect to a node we're already connected to {}",
                   address.toString());
        CLOG_ERROR(Overlay, "{}", REPORT_INTERNAL_BUG);
        return false;
    }
}

OverlayManagerImpl::PeersList&
OverlayManagerImpl::getPeersList(Peer* peer)
{
    ZoneScoped;
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
    ZoneScoped;
    auto type = setPreferred ? PeerType::PREFERRED : PeerType::OUTBOUND;
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
            getPeerManager().update(peer, type,
                                    /* preferredTypeKnown */ false,
                                    PeerManager::BackOffUpdate::HARD_RESET);
        }
        else
        {
            // If address is present in the DB, `update` will ensure
            // type is correctly updated. Otherwise, a new entry is created.
            // Note that this won't downgrade preferred peers back to outbound.
            getPeerManager().update(peer, type,
                                    /* preferredTypeKnown */ false);
        }
    }
}

void
OverlayManagerImpl::storeConfigPeers()
{
    ZoneScoped;
    // Synchronously resolve and store peers from the config
    storePeerList(resolvePeers(mApp.getConfig().KNOWN_PEERS), false, true);
    storePeerList(resolvePeers(mApp.getConfig().PREFERRED_PEERS), true, true);
}

void
OverlayManagerImpl::purgeDeadPeers()
{
    ZoneScoped;
    getPeerManager().removePeersWithManyFailures(
        Config::REALLY_DEAD_NUM_FAILURES_CUTOFF);
}

void
OverlayManagerImpl::triggerPeerResolution()
{
    ZoneScoped;
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
    ZoneScoped;
    std::vector<PeerBareAddress> addresses;
    addresses.reserve(peers.size());
    for (auto const& peer : peers)
    {
        try
        {
            addresses.push_back(PeerBareAddress::resolve(peer, mApp));
        }
        catch (std::runtime_error& e)
        {
            CLOG_ERROR(Overlay, "Unable to resolve peer '{}': {}", peer,
                       e.what());
            CLOG_ERROR(Overlay, "Peer may be no longer available under "
                                "this address. Please update your "
                                "PREFERRED_PEERS and KNOWN_PEERS "
                                "settings in configuration file");
        }
    }
    return addresses;
}

std::vector<PeerBareAddress>
OverlayManagerImpl::getPeersToConnectTo(int maxNum, PeerType peerType)
{
    ZoneScoped;
    assert(maxNum >= 0);
    if (maxNum == 0)
    {
        return {};
    }

    auto keep = [&](PeerBareAddress const& address) {
        auto peer = getConnectedPeer(address);
        auto promote = peer && (peerType == PeerType::INBOUND) &&
                       (peer->getRole() == Peer::REMOTE_CALLED_US);
        return !peer || promote;
    };

    // don't connect to too many peers at once
    return mPeerSources[peerType]->getRandomPeers(std::min(maxNum, 50), keep);
}

int
OverlayManagerImpl::connectTo(int maxNum, PeerType peerType)
{
    ZoneScoped;
    return connectTo(getPeersToConnectTo(maxNum, peerType),
                     peerType == PeerType::INBOUND);
}

int
OverlayManagerImpl::connectTo(std::vector<PeerBareAddress> const& peers,
                              bool forceoutbound)
{
    ZoneScoped;
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

// called every PEER_AUTHENTICATION_TIMEOUT + 1=3 seconds
void
OverlayManagerImpl::tick()
{
    ZoneScoped;
    CLOG_TRACE(Overlay, "OverlayManagerImpl tick  @{}",
               mApp.getConfig().PEER_PORT);

    if (futureIsReady(mResolvedPeers))
    {
        CLOG_TRACE(Overlay, "Resolved peers are ready");
        auto res = mResolvedPeers.get();
        storePeerList(res.known, false, false);
        storePeerList(res.preferred, true, false);
        mPeerIPTimer.expires_from_now(PEER_IP_RESOLVE_DELAY);
        mPeerIPTimer.async_wait([this]() { this->triggerPeerResolution(); },
                                VirtualTimer::onFailureNoop);
    }

    auto availablePendingSlots = availableOutboundPendingSlots();
    if (availablePendingSlots == 0)
    {
        // Exit early: no pending slots available
        return;
    }

    auto availableAuthenticatedSlots = availableOutboundAuthenticatedSlots();

    // First, connect to preferred peers
    {
        // in that context, an available slot is either a free slot or a non
        // preferred one
        int preferredToConnect =
            availableAuthenticatedSlots + nonPreferredAuthenticatedCount();
        preferredToConnect =
            std::min(availablePendingSlots, preferredToConnect);

        auto pendingUsedByPreferred =
            connectTo(preferredToConnect, PeerType::PREFERRED);

        assert(pendingUsedByPreferred <= availablePendingSlots);
        availablePendingSlots -= pendingUsedByPreferred;
    }

    // Second, if there is capacity for pending and authenticated outbound
    // connections, connect to more peers. Note: connect even if
    // PREFERRED_PEER_ONLY is set, to support key-based preferred peers mode
    // (see PREFERRED_PEER_KEYS). When PREFERRED_PEER_ONLY is set and we connect
    // to a non-preferred peer, drop it and backoff during handshake.
    if (availablePendingSlots > 0 && availableAuthenticatedSlots > 0)
    {
        // try to leave at least some pending slots for peer promotion
        constexpr const auto RESERVED_FOR_PROMOTION = 1;
        auto outboundToConnect =
            availablePendingSlots > RESERVED_FOR_PROMOTION
                ? std::min(availablePendingSlots - RESERVED_FOR_PROMOTION,
                           availableAuthenticatedSlots)
                : availablePendingSlots;
        auto pendingUsedByOutbound =
            connectTo(outboundToConnect, PeerType::OUTBOUND);
        assert(pendingUsedByOutbound <= availablePendingSlots);
        availablePendingSlots -= pendingUsedByOutbound;
    }

    // Finally, attempt to promote some inbound connections to outbound
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
            mOutboundPeers.mPending.size());
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
    auto outbound = mOutboundPeers.byAddress(address);
    return outbound ? outbound : mInboundPeers.byAddress(address);
}

void
OverlayManagerImpl::clearLedgersBelow(uint32_t ledgerSeq, uint32_t lclSeq)
{
    mFloodGate.clearBelow(ledgerSeq);
    mSurveyManager->clearOldLedgers(lclSeq);
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
    ZoneScoped;
    assert(peer->getRole() == Peer::REMOTE_CALLED_US);
    mInboundPeers.mConnectionsAttempted.Mark();

    auto haveSpace = mInboundPeers.mPending.size() <
                     mApp.getConfig().MAX_INBOUND_PENDING_CONNECTIONS;
    if (!haveSpace && mInboundPeers.mPending.size() <
                          mApp.getConfig().MAX_INBOUND_PENDING_CONNECTIONS +
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
            CLOG_DEBUG(
                Overlay,
                "Peer rejected - all pending inbound connections are taken: {}",
                peer->toString());
            CLOG_DEBUG(Overlay, "If you wish to allow for more pending "
                                "inbound connections, please update your "
                                "MAX_PENDING_CONNECTIONS setting in "
                                "configuration file.");
        }

        mInboundPeers.mConnectionsCancelled.Mark();
        peer->drop("all pending inbound connections are taken",
                   Peer::DropDirection::WE_DROPPED_REMOTE,
                   Peer::DropMode::IGNORE_WRITE_QUEUE);
        return;
    }
    CLOG_DEBUG(Overlay, "New (inbound) connected peer {} @{}", peer->toString(),
               mApp.getConfig().PEER_PORT);
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
    ZoneScoped;
    assert(peer->getRole() == Peer::WE_CALLED_REMOTE);
    mOutboundPeers.mConnectionsAttempted.Mark();

    if (mShuttingDown || mOutboundPeers.mPending.size() >=
                             mApp.getConfig().MAX_OUTBOUND_PENDING_CONNECTIONS)
    {
        if (!mShuttingDown)
        {
            CLOG_DEBUG(Overlay,
                       "Peer rejected - all outbound connections taken: {} @{}",
                       peer->toString(), mApp.getConfig().PEER_PORT);
            CLOG_DEBUG(Overlay, "If you wish to allow for more pending "
                                "outbound connections, please update "
                                "your MAX_PENDING_CONNECTIONS setting in "
                                "configuration file.");
        }

        mOutboundPeers.mConnectionsCancelled.Mark();
        peer->drop("all outbound connections taken",
                   Peer::DropDirection::WE_DROPPED_REMOTE,
                   Peer::DropMode::IGNORE_WRITE_QUEUE);
        return false;
    }
    CLOG_DEBUG(Overlay, "New (outbound) connected peer {} @{}",
               peer->toString(), mApp.getConfig().PEER_PORT);
    mOutboundPeers.mConnectionsEstablished.Mark();
    mOutboundPeers.mPending.push_back(peer);
    updateSizeCounters();

    return true;
}

void
OverlayManagerImpl::removePeer(Peer* peer)
{
    ZoneScoped;
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
OverlayManagerImpl::acceptAuthenticatedPeer(Peer::pointer peer)
{
    return getPeersList(peer.get()).acceptAuthenticatedPeer(peer);
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

std::map<NodeID, Peer::pointer> const&
OverlayManagerImpl::getInboundAuthenticatedPeers() const
{
    return mInboundPeers.mAuthenticated;
}

std::map<NodeID, Peer::pointer> const&
OverlayManagerImpl::getOutboundAuthenticatedPeers() const
{
    return mOutboundPeers.mAuthenticated;
}

std::map<NodeID, Peer::pointer>
OverlayManagerImpl::getAuthenticatedPeers() const
{
    auto result = mOutboundPeers.mAuthenticated;
    result.insert(std::begin(mInboundPeers.mAuthenticated),
                  std::end(mInboundPeers.mAuthenticated));
    return result;
}

int
OverlayManagerImpl::getPendingPeersCount() const
{
    return static_cast<int>(mInboundPeers.mPending.size() +
                            mOutboundPeers.mPending.size());
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
        CLOG_DEBUG(Overlay, "Peer {} is preferred  @{}", pstr,
                   mApp.getConfig().PEER_PORT);
        return true;
    }

    if (peer->isAuthenticated())
    {
        if (mApp.getConfig().PREFERRED_PEER_KEYS.count(peer->getPeerID()) != 0)
        {
            CLOG_DEBUG(Overlay, "Peer key {} is preferred @{}",
                       mApp.getConfig().toShortString(peer->getPeerID()),
                       mApp.getConfig().PEER_PORT);
            return true;
        }
    }

    CLOG_TRACE(Overlay, "Peer {} is not preferred @{}", pstr,
               mApp.getConfig().PEER_PORT);
    return false;
}

std::vector<Peer::pointer>
OverlayManagerImpl::getRandomAuthenticatedPeers()
{
    std::vector<Peer::pointer> result;
    result.reserve(mInboundPeers.mAuthenticated.size() +
                   mOutboundPeers.mAuthenticated.size());
    extractPeersFromMap(mInboundPeers.mAuthenticated, result);
    extractPeersFromMap(mOutboundPeers.mAuthenticated, result);
    shufflePeerList(result);
    return result;
}

std::vector<Peer::pointer>
OverlayManagerImpl::getRandomInboundAuthenticatedPeers()
{
    std::vector<Peer::pointer> result;
    result.reserve(mInboundPeers.mAuthenticated.size());
    extractPeersFromMap(mInboundPeers.mAuthenticated, result);
    shufflePeerList(result);
    return result;
}

std::vector<Peer::pointer>
OverlayManagerImpl::getRandomOutboundAuthenticatedPeers()
{
    std::vector<Peer::pointer> result;
    result.reserve(mOutboundPeers.mAuthenticated.size());
    extractPeersFromMap(mOutboundPeers.mAuthenticated, result);
    shufflePeerList(result);
    return result;
}

void
OverlayManagerImpl::extractPeersFromMap(
    std::map<NodeID, Peer::pointer> const& peerMap,
    std::vector<Peer::pointer>& result)
{
    auto extractPeer = [](std::pair<NodeID, Peer::pointer> const& peer) {
        return peer.second;
    };
    std::transform(std::begin(peerMap), std::end(peerMap),
                   std::back_inserter(result), extractPeer);
}

void
OverlayManagerImpl::shufflePeerList(std::vector<Peer::pointer>& peerList)
{
    std::shuffle(peerList.begin(), peerList.end(), gRandomEngine);
}

bool
OverlayManagerImpl::recvFloodedMsgID(StellarMessage const& msg,
                                     Peer::pointer peer, Hash& msgID)
{
    ZoneScoped;
    return mFloodGate.addRecord(msg, peer, msgID);
}

void
OverlayManagerImpl::forgetFloodedMsg(Hash const& msgID)
{
    ZoneScoped;
    mFloodGate.forgetRecord(msgID);
}

bool
OverlayManagerImpl::broadcastMessage(StellarMessage const& msg, bool force)
{
    ZoneScoped;
    auto res = mFloodGate.broadcast(msg, force);
    if (res)
    {
        mOverlayMetrics.mMessagesBroadcast.Mark();
    }
    return res;
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

PeerManager&
OverlayManagerImpl::getPeerManager()
{
    return mPeerManager;
}

SurveyManager&
OverlayManagerImpl::getSurveyManager()
{
    return *mSurveyManager;
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

void
OverlayManagerImpl::recordMessageMetric(StellarMessage const& stellarMsg,
                                        Peer::pointer peer)
{
    ZoneScoped;
    auto logMessage = [&](bool unique, std::string const& msgType) {
        if (Logging::logTrace("Overlay"))
        {
            CLOG_TRACE(Overlay, "recv: {} {} ({}) of size: {} from: {} @{}",
                       (unique ? "unique" : "duplicate"),
                       peer->msgSummary(stellarMsg), msgType,
                       xdr::xdr_argpack_size(stellarMsg),
                       mApp.getConfig().toShortString(peer->getPeerID()),
                       mApp.getConfig().PEER_PORT);
        }
    };

    bool flood = false;
    if (stellarMsg.type() == TRANSACTION || stellarMsg.type() == SCP_MESSAGE ||
        stellarMsg.type() == SURVEY_REQUEST ||
        stellarMsg.type() == SURVEY_RESPONSE)
    {
        flood = true;
    }
    else if (stellarMsg.type() != TX_SET && stellarMsg.type() != SCP_QUORUMSET)
    {
        return;
    }

    auto& peerMetrics = peer->getPeerMetrics();

    size_t size = xdr::xdr_argpack_size(stellarMsg);
    auto hash = shortHash::xdrComputeHash(stellarMsg);
    if (mMessageCache.exists(hash))
    {
        if (flood)
        {
            mOverlayMetrics.mDuplicateFloodBytesRecv.Mark(size);

            peerMetrics.mDuplicateFloodBytesRecv += size;
            ++peerMetrics.mDuplicateFloodMessageRecv;

            logMessage(false, "flood");
        }
        else
        {
            mOverlayMetrics.mDuplicateFetchBytesRecv.Mark(size);

            peerMetrics.mDuplicateFetchBytesRecv += size;
            ++peerMetrics.mDuplicateFetchMessageRecv;

            logMessage(false, "fetch");
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

            peerMetrics.mUniqueFloodBytesRecv += size;
            ++peerMetrics.mUniqueFloodMessageRecv;

            logMessage(true, "flood");
        }
        else
        {
            mOverlayMetrics.mUniqueFetchBytesRecv.Mark(size);

            peerMetrics.mUniqueFetchBytesRecv += size;
            ++peerMetrics.mUniqueFetchMessageRecv;

            logMessage(true, "fetch");
        }
    }
}

void
OverlayManagerImpl::updateFloodRecord(StellarMessage const& oldMsg,
                                      StellarMessage const& newMsg)
{
    ZoneScoped;
    mFloodGate.updateRecord(oldMsg, newMsg);
}
}
