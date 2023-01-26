// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/OverlayManagerImpl.h"
#include "crypto/Hex.h"
#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "crypto/ShortHash.h"
#include "database/Database.h"
#include "herder/Herder.h"
#include "ledger/LedgerManager.h"
#include "lib/util/finally.h"
#include "lib/util/stdrandom.h"
#include "main/Application.h"
#include "main/Config.h"
#include "main/ErrorMessages.h"
#include "overlay/OverlayMetrics.h"
#include "overlay/PeerBareAddress.h"
#include "overlay/PeerManager.h"
#include "overlay/RandomPeerSource.h"
#include "overlay/TCPPeer.h"
#include "util/GlobalChecks.h"
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
constexpr std::chrono::seconds PEER_IP_RESOLVE_RETRY_DELAY(10);
constexpr std::chrono::seconds OUT_OF_SYNC_RECONNECT_DELAY(60);

// Regardless of the number of failed attempts &
// FLOOD_DEMAND_BACKOFF_DELAY_MS it doesn't make much sense to wait much
// longer than 2 seconds between re-issuing demands.
constexpr std::chrono::seconds MAX_DELAY_DEMAND{2};

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
    CLOG_TRACE(Overlay, "Removing peer {}", peer->toString());
    releaseAssert(peer->getState() == Peer::CLOSING);

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
    CLOG_TRACE(Overlay, "Moving peer {} to authenticated  state: {}",
               peer->toString(), peer->getState());
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
    CLOG_TRACE(Overlay, "Trying to promote peer to authenticated {}",
               peer->toString());
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
    , mDemandTimer(app)
    , mResolvingPeersWithBackoff(true)
    , mResolvingPeersRetryCount(0)

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
    // Start demanding.
    demand();
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
    CLOG_TRACE(Overlay, "Connect to {}", address.toString());
    auto currentConnection = getConnectedPeer(address);
    if (!currentConnection || (forceoutbound && currentConnection->getRole() ==
                                                    Peer::REMOTE_CALLED_US))
    {
        if (availableOutboundPendingSlots() <= 0)
        {
            CLOG_DEBUG(Overlay,
                       "Peer rejected - all outbound pending connections "
                       "taken: {}",
                       address.toString());
            return false;
        }
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
    storePeerList(resolvePeers(mApp.getConfig().KNOWN_PEERS).first, false,
                  true);
    storePeerList(resolvePeers(mApp.getConfig().PREFERRED_PEERS).first, true,
                  true);
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
    releaseAssert(!mResolvedPeers.valid());

    // Trigger DNS resolution on the background thread
    using task_t = std::packaged_task<ResolvedPeers()>;
    std::shared_ptr<task_t> task = std::make_shared<task_t>([this]() {
        if (!this->mShuttingDown)
        {
            auto known = resolvePeers(this->mApp.getConfig().KNOWN_PEERS);
            auto preferred =
                resolvePeers(this->mApp.getConfig().PREFERRED_PEERS);
            return ResolvedPeers{known.first, preferred.first,
                                 known.second || preferred.second};
        }
        return ResolvedPeers{{}, {}, false};
    });

    mResolvedPeers = task->get_future();
    mApp.postOnBackgroundThread(bind(&task_t::operator(), task),
                                "OverlayManager: resolve peer IPs");
}

std::pair<std::vector<PeerBareAddress>, bool>
OverlayManagerImpl::resolvePeers(std::vector<string> const& peers)
{
    ZoneScoped;
    std::vector<PeerBareAddress> addresses;
    addresses.reserve(peers.size());
    bool errors = false;
    for (auto const& peer : peers)
    {
        try
        {
            addresses.push_back(PeerBareAddress::resolve(peer, mApp));
        }
        catch (std::runtime_error& e)
        {
            errors = true;
            CLOG_ERROR(Overlay, "Unable to resolve peer '{}': {}", peer,
                       e.what());
            CLOG_ERROR(Overlay, "Peer may be no longer available under "
                                "this address. Please update your "
                                "PREFERRED_PEERS and KNOWN_PEERS "
                                "settings in configuration file");
        }
    }
    return std::make_pair(addresses, errors);
}

std::vector<PeerBareAddress>
OverlayManagerImpl::getPeersToConnectTo(int maxNum, PeerType peerType)
{
    ZoneScoped;
    releaseAssert(maxNum >= 0);
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

void
OverlayManagerImpl::updateTimerAndMaybeDropRandomPeer(bool shouldDrop)
{
    // If we haven't heard from the network for a while, try randomly
    // disconnecting a peer in hopes of picking a better one. (preferred peers
    // aren't affected as we always want to stay connected)
    auto now = mApp.getClock().now();
    if (!mApp.getHerder().isTracking())
    {
        if (mLastOutOfSyncReconnect)
        {
            // We've been out of sync, check if it's time to drop a peer
            if (now - *mLastOutOfSyncReconnect > OUT_OF_SYNC_RECONNECT_DELAY &&
                shouldDrop)
            {
                auto allPeers = getOutboundAuthenticatedPeers();
                std::vector<std::pair<NodeID, Peer::pointer>> nonPreferredPeers;
                std::copy_if(std::begin(allPeers), std::end(allPeers),
                             std::back_inserter(nonPreferredPeers),
                             [&](auto const& peer) {
                                 return !mApp.getOverlayManager().isPreferred(
                                     peer.second.get());
                             });
                if (!nonPreferredPeers.empty())
                {
                    auto peerToDrop = rand_element(nonPreferredPeers);
                    peerToDrop.second->sendErrorAndDrop(
                        ERR_LOAD, "random disconnect due to out of sync",
                        Peer::DropMode::IGNORE_WRITE_QUEUE);
                }
                // Reset the timer to throttle dropping peers
                mLastOutOfSyncReconnect =
                    std::make_optional<VirtualClock::time_point>(now);
            }
            else
            {
                // Still waiting for the timeout or outbound capacity
                return;
            }
        }
        else
        {
            // Start a timer after going out of sync. Note that we still want to
            // wait for OUT_OF_SYNC_RECONNECT_DELAY for Herder recovery logic to
            // trigger.
            mLastOutOfSyncReconnect =
                std::make_optional<VirtualClock::time_point>(now);
        }
    }
    else
    {
        // Reset timer when in-sync
        mLastOutOfSyncReconnect.reset();
    }
}

// called every PEER_AUTHENTICATION_TIMEOUT + 1=3 seconds
void
OverlayManagerImpl::tick()
{
    ZoneScoped;
    CLOG_TRACE(Overlay, "OverlayManagerImpl tick");

    auto rescheduleTick = gsl::finally([&]() {
        mTimer.expires_from_now(std::chrono::seconds(
            mApp.getConfig().PEER_AUTHENTICATION_TIMEOUT + 1));
        mTimer.async_wait([this]() { this->tick(); },
                          VirtualTimer::onFailureNoop);
    });

    if (futureIsReady(mResolvedPeers))
    {
        CLOG_TRACE(Overlay, "Resolved peers are ready");
        auto res = mResolvedPeers.get();
        storePeerList(res.known, false, false);
        storePeerList(res.preferred, true, false);
        std::chrono::seconds retryDelay = PEER_IP_RESOLVE_DELAY;

        if (mResolvingPeersWithBackoff)
        {
            // no errors -> disable retries completely from now on
            if (!res.errors)
            {
                mResolvingPeersWithBackoff = false;
            }
            else
            {
                ++mResolvingPeersRetryCount;
                auto newDelay =
                    mResolvingPeersRetryCount * PEER_IP_RESOLVE_RETRY_DELAY;
                // if we retried too many times, give up on retries
                if (newDelay > PEER_IP_RESOLVE_DELAY)
                {
                    mResolvingPeersWithBackoff = false;
                }
                else
                {
                    retryDelay = newDelay;
                }
            }
        }

        mPeerIPTimer.expires_from_now(retryDelay);
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

        releaseAssert(pendingUsedByPreferred <= availablePendingSlots);
        availablePendingSlots -= pendingUsedByPreferred;
    }

    // Only trigger reconnecting if:
    //   * no outbound slots are available
    //   * we didn't establish any new preferred peers connections (those
    //      will evict regular peers anyway)
    bool shouldDrop =
        availableAuthenticatedSlots == 0 && availablePendingSlots > 0;
    updateTimerAndMaybeDropRandomPeer(shouldDrop);

    availableAuthenticatedSlots = availableOutboundAuthenticatedSlots();

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
        releaseAssert(pendingUsedByOutbound <= availablePendingSlots);
        availablePendingSlots -= pendingUsedByOutbound;
    }

    // Finally, attempt to promote some inbound connections to outbound
    if (availablePendingSlots > 0)
    {
        connectTo(availablePendingSlots, PeerType::INBOUND);
    }
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
    auto adjustedTarget =
        mInboundPeers.mAuthenticated.size() == 0 &&
                !mApp.getConfig()
                     .ARTIFICIALLY_SKIP_CONNECTION_ADJUSTMENT_FOR_TESTING
            ? OverlayManager::MIN_INBOUND_FACTOR
            : mApp.getConfig().TARGET_PEER_CONNECTIONS;

    if (mOutboundPeers.mAuthenticated.size() < adjustedTarget)
    {
        return static_cast<int>(adjustedTarget -
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

    releaseAssert(nonPreferredCount <=
                  mApp.getConfig().TARGET_PEER_CONNECTIONS);
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
    for (auto const& peer : getAuthenticatedPeers())
    {
        peer.second->clearBelow(ledgerSeq);
    }
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
    releaseAssert(peer->getRole() == Peer::REMOTE_CALLED_US);
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
    CLOG_DEBUG(Overlay, "New (inbound) connected peer {}", peer->toString());
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
    releaseAssert(peer->getRole() == Peer::WE_CALLED_REMOTE);
    mOutboundPeers.mConnectionsAttempted.Mark();

    if (mShuttingDown || availableOutboundPendingSlots() <= 0)
    {
        if (!mShuttingDown)
        {
            CLOG_DEBUG(Overlay,
                       "Peer rejected - all outbound connections taken: {}",
                       peer->toString());
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
    CLOG_DEBUG(Overlay, "New (outbound) connected peer {}", peer->toString());
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
        CLOG_DEBUG(Overlay, "Peer {} is preferred", pstr);
        return true;
    }

    if (peer->isAuthenticated())
    {
        if (mApp.getConfig().PREFERRED_PEER_KEYS.count(peer->getPeerID()) != 0)
        {
            CLOG_DEBUG(Overlay, "Peer key {} is preferred",
                       mApp.getConfig().toShortString(peer->getPeerID()));
            return true;
        }
    }

    CLOG_TRACE(Overlay, "Peer {} is not preferred", pstr);
    return false;
}

bool
OverlayManagerImpl::isFloodMessage(StellarMessage const& msg)
{
    return msg.type() == SCP_MESSAGE || msg.type() == TRANSACTION ||
           msg.type() == FLOOD_DEMAND || msg.type() == FLOOD_ADVERT;
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
    stellar::shuffle(peerList.begin(), peerList.end(), gRandomEngine);
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
OverlayManagerImpl::broadcastMessage(StellarMessage const& msg, bool force,
                                     std::optional<Hash> const hash)
{
    ZoneScoped;
    auto res = mFloodGate.broadcast(msg, force, hash);
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

    mDemandTimer.cancel();

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
        CLOG_TRACE(Overlay, "recv: {} {} ({}) of size: {} from: {}",
                   (unique ? "unique" : "duplicate"),
                   peer->msgSummary(stellarMsg), msgType,
                   xdr::xdr_argpack_size(stellarMsg),
                   mApp.getConfig().toShortString(peer->getPeerID()));
    };

    bool flood = false;
    if (isFloodMessage(stellarMsg) || stellarMsg.type() == SURVEY_REQUEST ||
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

size_t
OverlayManagerImpl::getMaxAdvertSize() const
{
    auto const& cfg = mApp.getConfig();
    auto ledgerCloseTime =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            cfg.getExpectedLedgerCloseTime())
            .count();
    double opRatePerLedger = cfg.FLOOD_OP_RATE_PER_LEDGER;
    size_t maxOps = mApp.getLedgerManager().getLastMaxTxSetSizeOps();
    double opsToFloodPerLedgerDbl =
        opRatePerLedger * static_cast<double>(maxOps);
    releaseAssertOrThrow(opsToFloodPerLedgerDbl >= 0.0);
    int64_t opsToFloodPerLedger = static_cast<int64_t>(opsToFloodPerLedgerDbl);

    size_t res = static_cast<size_t>(bigDivideOrThrow(
        opsToFloodPerLedger, cfg.FLOOD_ADVERT_PERIOD_MS.count(),
        ledgerCloseTime, Rounding::ROUND_UP));

    res = std::max<size_t>(1, res);
    res = std::min<size_t>(TX_ADVERT_VECTOR_MAX_SIZE, res);
    return res;
}

size_t
OverlayManagerImpl::getMaxDemandSize() const
{
    auto const& cfg = mApp.getConfig();
    auto ledgerCloseTime =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            cfg.getExpectedLedgerCloseTime())
            .count();
    double opRatePerLedger = cfg.FLOOD_OP_RATE_PER_LEDGER;
    double queueSizeInOpsDbl =
        static_cast<double>(mApp.getHerder().getMaxQueueSizeOps()) *
        opRatePerLedger;
    releaseAssertOrThrow(queueSizeInOpsDbl >= 0.0);
    int64_t queueSizeInOps = static_cast<int64_t>(queueSizeInOpsDbl);

    size_t res = static_cast<size_t>(
        bigDivideOrThrow(queueSizeInOps, cfg.FLOOD_DEMAND_PERIOD_MS.count(),
                         ledgerCloseTime, Rounding::ROUND_UP));
    res = std::max<size_t>(1, res);
    res = std::min<size_t>(TX_DEMAND_VECTOR_MAX_SIZE, res);
    return res;
}

void
OverlayManagerImpl::recordTxPullLatency(Hash const& hash,
                                        std::shared_ptr<Peer> peer)
{
    auto it = mDemandHistoryMap.find(hash);
    auto now = mApp.getClock().now();
    if (it != mDemandHistoryMap.end())
    {
        // Record end-to-end pull time
        if (!it->second.latencyRecorded)
        {
            auto delta = now - it->second.firstDemanded;
            mOverlayMetrics.mTxPullLatency.Update(delta);
            it->second.latencyRecorded = true;
            CLOG_DEBUG(
                Overlay,
                "Pulled transaction {} in {} milliseconds, asked {} peers",
                hexAbbrev(hash),
                std::chrono::duration_cast<std::chrono::milliseconds>(delta)
                    .count(),
                it->second.peers.size());
        }

        // Record pull time from individual peer
        auto peerIt = it->second.peers.find(peer->getPeerID());
        if (peerIt != it->second.peers.end())
        {
            auto delta = now - peerIt->second;
            mOverlayMetrics.mPeerTxPullLatency.Update(delta);
            peer->getPeerMetrics().mPullLatency.Update(delta);
            CLOG_DEBUG(
                Overlay,
                "Pulled transaction {} in {} milliseconds from peer {}",
                hexAbbrev(hash),
                std::chrono::duration_cast<std::chrono::milliseconds>(delta)
                    .count(),
                peer->toString());
        }
    }
}

std::chrono::milliseconds
OverlayManagerImpl::retryDelayDemand(int numAttemptsMade) const
{
    auto res = numAttemptsMade * mApp.getConfig().FLOOD_DEMAND_BACKOFF_DELAY_MS;
    return std::min(res, std::chrono::milliseconds(MAX_DELAY_DEMAND));
}

OverlayManagerImpl::DemandStatus
OverlayManagerImpl::demandStatus(Hash const& txHash, Peer::pointer peer) const
{
    if (mApp.getHerder().isBannedTx(txHash) ||
        mApp.getHerder().getTx(txHash) != nullptr)
    {
        return DemandStatus::DISCARD;
    }
    auto it = mDemandHistoryMap.find(txHash);
    if (it == mDemandHistoryMap.end())
    {
        // never demanded
        return DemandStatus::DEMAND;
    }
    auto& demandedPeers = it->second.peers;
    if (demandedPeers.find(peer->getPeerID()) != demandedPeers.end())
    {
        // We've already demanded.
        return DemandStatus::DISCARD;
    }
    int const numDemanded = static_cast<int>(demandedPeers.size());
    auto const lastDemanded = it->second.lastDemanded;

    if (numDemanded < MAX_RETRY_COUNT)
    {
        // Check if it's been a while since our last demand
        if ((mApp.getClock().now() - lastDemanded) >=
            retryDelayDemand(numDemanded))
        {
            return DemandStatus::DEMAND;
        }
        else
        {
            return DemandStatus::RETRY_LATER;
        }
    }
    return DemandStatus::DISCARD;
}

void
OverlayManagerImpl::demand()
{
    ZoneScoped;
    if (mShuttingDown)
    {
        return;
    }
    auto const now = mApp.getClock().now();

    // We determine that demands are obsolete after maxRetention.
    auto maxRetention = MAX_DELAY_DEMAND * MAX_RETRY_COUNT * 2;
    while (!mPendingDemands.empty())
    {
        auto const& it = mDemandHistoryMap.find(mPendingDemands.front());
        if ((now - it->second.firstDemanded) >= maxRetention)
        {
            if (!it->second.latencyRecorded)
            {
                // We never received the txn.
                mOverlayMetrics.mAbandonedDemandMeter.Mark();
            }
            mPendingDemands.pop();
            mDemandHistoryMap.erase(it);
        }
        else
        {
            // The oldest demand in mPendingDemands isn't old enough
            // to be deleted from our record.
            break;
        }
    }

    auto peers = getRandomAuthenticatedPeers();

    auto const& cfg = mApp.getConfig();

    UnorderedMap<Peer::pointer, std::pair<TxDemandVector, std::list<Hash>>>
        demandMap;
    bool anyNewDemand = false;
    do
    {
        anyNewDemand = false;
        for (auto const& peer : peers)
        {
            auto& demPair = demandMap[peer];
            auto& demand = demPair.first;
            auto& retry = demPair.second;
            bool addedNewDemand = false;
            while (demand.size() < getMaxDemandSize() &&
                   peer->getTxAdvertQueue().size() > 0 && !addedNewDemand)
            {
                auto hashPair = peer->getTxAdvertQueue().pop();
                auto txHash = hashPair.first;
                if (hashPair.second)
                {
                    auto delta = now - *(hashPair.second);
                    mOverlayMetrics.mAdvertQueueDelay.Update(delta);
                    peer->getPeerMetrics().mAdvertQueueDelay.Update(delta);
                }

                switch (demandStatus(txHash, peer))
                {
                case DemandStatus::DEMAND:
                    demand.push_back(txHash);
                    if (mDemandHistoryMap.find(txHash) ==
                        mDemandHistoryMap.end())
                    {
                        // We don't have any pending demand record of this tx
                        // hash.
                        mPendingDemands.push(txHash);
                        mDemandHistoryMap[txHash].firstDemanded = now;
                        CLOG_DEBUG(Overlay, "Demand tx {}, asking peer {}",
                                   hexAbbrev(txHash), peer->toString());
                    }
                    else
                    {
                        getOverlayMetrics().mDemandTimeouts.Mark();
                        ++(peer->getPeerMetrics().mDemandTimeouts);
                        CLOG_DEBUG(Overlay, "Timeout for tx {}, asking peer {}",
                                   hexAbbrev(txHash), peer->toString());
                    }
                    mDemandHistoryMap[txHash].peers.emplace(peer->getPeerID(),
                                                            now);
                    mDemandHistoryMap[txHash].lastDemanded = now;
                    addedNewDemand = true;
                    break;
                case DemandStatus::RETRY_LATER:
                    retry.push_back(txHash);
                    break;
                case DemandStatus::DISCARD:
                    break;
                }
            }
            anyNewDemand |= addedNewDemand;
        }
    } while (anyNewDemand);

    for (auto const& peer : peers)
    {
        // We move `demand` here and also pass `retry` as a reference
        // which gets appended. Don't touch `demand` or `retry` after here.
        peer->sendTxDemand(std::move(demandMap[peer].first));
        peer->getTxAdvertQueue().appendHashesToRetryAndMaybeTrim(
            demandMap[peer].second);
    }

    // mPendingDemands and mDemandHistoryMap must always contain exactly the
    // same tx hashes.
    releaseAssert(mPendingDemands.size() == mDemandHistoryMap.size());

    mDemandTimer.expires_from_now(cfg.FLOOD_DEMAND_PERIOD_MS);
    mDemandTimer.async_wait([this](asio::error_code const& error) {
        if (!error)
        {
            this->demand();
        }
    });
}

}
