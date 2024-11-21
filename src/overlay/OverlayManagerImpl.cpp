// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/OverlayManagerImpl.h"
#include "crypto/Hex.h"
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
#include "overlay/SurveyDataManager.h"
#include "overlay/TCPPeer.h"
#include "overlay/TxDemandsManager.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/Thread.h"
#include "xdrpp/marshal.h"
#include <Tracy.hpp>
#include <fmt/format.h>

#include "medida/counter.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"

#include <algorithm>

namespace stellar
{

using namespace soci;
using namespace std;

constexpr std::chrono::seconds PEER_IP_RESOLVE_DELAY(600);
constexpr std::chrono::seconds PEER_IP_RESOLVE_RETRY_DELAY(10);
constexpr std::chrono::seconds OUT_OF_SYNC_RECONNECT_DELAY(60);
constexpr uint32_t INITIAL_PEER_FLOOD_READING_CAPACITY_BYTES{300000};
constexpr uint32_t INITIAL_FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES{100000};

bool
OverlayManagerImpl::canAcceptOutboundPeer(PeerBareAddress const& address) const
{
    if (availableOutboundPendingSlots() <= 0)
    {
        CLOG_DEBUG(Overlay,
                   "Peer rejected - all outbound pending connections "
                   "taken: {}",
                   address.toString());
        CLOG_DEBUG(Overlay, "If you wish to allow for more pending "
                            "outbound connections, please update "
                            "your MAX_PENDING_CONNECTIONS setting in "
                            "configuration file.");
        return false;
    }
    if (mShuttingDown)
    {
        CLOG_DEBUG(Overlay, "Peer rejected - overlay shutting down: {}",
                   address.toString());
        return false;
    }
    return true;
}

OverlayManagerImpl::PeersList::PeersList(
    OverlayManagerImpl& overlayManager,
    medida::MetricsRegistry& metricsRegistry,
    std::string const& directionString, std::string const& cancelledName,
    int maxAuthenticatedCount, std::shared_ptr<SurveyManager> sm)
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
    , mSurveyManager(sm)
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
    peer->assertShuttingDown();

    auto pendingIt =
        std::find_if(std::begin(mPending), std::end(mPending),
                     [&](Peer::pointer const& p) { return p.get() == peer; });
    if (pendingIt != std::end(mPending))
    {
        CLOG_TRACE(Overlay, "Dropping pending {} peer: {}", mDirectionString,
                   peer->toString());
        // Prolong the lifetime of dropped peer for a bit until background
        // thread is done processing it
        mDropped.insert(*pendingIt);
        mPending.erase(pendingIt);
        mConnectionsDropped.Mark();
        return;
    }

    auto authentiatedIt = mAuthenticated.find(peer->getPeerID());
    if (authentiatedIt != std::end(mAuthenticated))
    {
        CLOG_DEBUG(Overlay, "Dropping authenticated {} peer: {}",
                   mDirectionString, peer->toString());
        // Prolong the lifetime of dropped peer for a bit until background
        // thread is done processing it
        mDropped.insert(authentiatedIt->second);
        mAuthenticated.erase(authentiatedIt);
        mConnectionsDropped.Mark();
        mSurveyManager->recordDroppedPeer(*peer);
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
    releaseAssert(threadIsMain());

    CLOG_TRACE(Overlay, "Moving peer {} to authenticated  state",
               peer->toString());
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

    CLOG_INFO(Overlay, "Authenticated to {}", peer->toString());

    mSurveyManager->modifyNodeData([&](CollectingNodeData& nodeData) {
        ++nodeData.mAddedAuthenticatedPeers;
    });

    return true;
}

bool
OverlayManagerImpl::PeersList::acceptAuthenticatedPeer(Peer::pointer peer)
{
    ZoneScoped;
    releaseAssert(threadIsMain());

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
                    ERR_LOAD, "preferred peer selected instead");
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
        p->sendErrorAndDrop(ERR_MISC, "shutdown");
    }
    auto authenticatedPeersToStop = mAuthenticated;
    for (auto& p : authenticatedPeersToStop)
    {
        p.second->sendErrorAndDrop(ERR_MISC, "shutdown");
    }

    for (auto& p : mDropped)
    {
        p->assertShuttingDown();
    }
}

std::unique_ptr<OverlayManager>
OverlayManager::create(Application& app)
{
    return std::make_unique<OverlayManagerImpl>(app);
}

OverlayManagerImpl::OverlayManagerImpl(Application& app)
    : mApp(app)
    , mLiveInboundPeersCounter(make_shared<int>(0))
    , mPeerManager(app)
    , mDoor(mApp)
    , mAuth(mApp)
    , mShuttingDown(false)
    , mOverlayMetrics(app)
    , mMessageCache(0xffff)
    , mTimer(app)
    , mPeerIPTimer(app)
    , mFloodGate(app)
    , mTxDemandsManager(app)
    , mSurveyManager(make_shared<SurveyManager>(app))
    , mInboundPeers(*this, mApp.getMetrics(), "inbound", "reject",
                    mApp.getConfig().MAX_ADDITIONAL_PEER_CONNECTIONS,
                    mSurveyManager)
    , mOutboundPeers(*this, mApp.getMetrics(), "outbound", "cancel",
                     mApp.getConfig().TARGET_PEER_CONNECTIONS, mSurveyManager)
    , mResolvingPeersWithBackoff(true)
    , mResolvingPeersRetryCount(0)
    , mScheduledMessages(100000, true)
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

    // Start demand logic
    mTxDemandsManager.start();
}

uint32_t
OverlayManagerImpl::getFlowControlBytesTotal() const
{
    releaseAssert(threadIsMain());
    auto const maxTxSize = mApp.getHerder().getMaxTxSize();
    releaseAssert(maxTxSize > 0);
    auto const& cfg = mApp.getConfig();

    // If flow control parameters weren't provided in the config file, calculate
    // them automatically using initial values, but adjusting them according to
    // maximum transactions byte size.
    if (cfg.PEER_FLOOD_READING_CAPACITY_BYTES == 0 &&
        cfg.FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES == 0)
    {
        if (!(INITIAL_PEER_FLOOD_READING_CAPACITY_BYTES -
                  INITIAL_FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES >=
              maxTxSize))
        {
            return maxTxSize + INITIAL_FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES;
        }
        return INITIAL_PEER_FLOOD_READING_CAPACITY_BYTES;
    }

    // If flow control parameters were provided, return them
    return cfg.PEER_FLOOD_READING_CAPACITY_BYTES;
}

uint32_t
OverlayManager::getFlowControlBytesBatch(Config const& cfg)
{
    if (cfg.PEER_FLOOD_READING_CAPACITY_BYTES == 0 &&
        cfg.FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES == 0)
    {
        return INITIAL_FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES;
    }

    // If flow control parameters were provided, return them
    return cfg.FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES;
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
    releaseAssert(threadIsMain());
    CLOG_TRACE(Overlay, "Initiate connect to {}", address.toString());
    auto currentConnection = getConnectedPeer(address);
    if (!currentConnection || (forceoutbound && currentConnection->getRole() ==
                                                    Peer::REMOTE_CALLED_US))
    {
        if (!canAcceptOutboundPeer(address))
        {
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
    std::shared_ptr<task_t> task =
        std::make_shared<task_t>([this, cfg = mApp.getConfig()]() {
            if (!this->mShuttingDown)
            {
                auto known = resolvePeers(cfg.KNOWN_PEERS);
                auto preferred = resolvePeers(cfg.PREFERRED_PEERS);
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
                        ERR_LOAD, "random disconnect due to out of sync");
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

    // Cleanup unreferenced peers.
    auto cleanupPeers = [](auto& peerList) {
        for (auto it = peerList.mDropped.begin();
             it != peerList.mDropped.end();)
        {
            auto const& p = *it;
            p->assertShuttingDown();
            if (p.use_count() == 1)
            {
                it = peerList.mDropped.erase(it);
            }
            else
            {
                ++it;
            }
        }
    };

    cleanupPeers(mInboundPeers);
    cleanupPeers(mOutboundPeers);

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

    // Check and update the overlay survey state
    mSurveyManager->updateSurveyPhase(getInboundAuthenticatedPeers(),
                                      getOutboundAuthenticatedPeers(),
                                      mApp.getConfig());

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
OverlayManagerImpl::maybeAddInboundConnection(Peer::pointer peer)
{
    ZoneScoped;
    mInboundPeers.mConnectionsAttempted.Mark();

    if (peer)
    {
        releaseAssert(peer->getRole() == Peer::REMOTE_CALLED_US);
        bool haveSpace = haveSpaceForConnection(peer->getAddress().getIP());

        if (mShuttingDown || !haveSpace)
        {
            mInboundPeers.mConnectionsCancelled.Mark();
            peer->drop("all pending inbound connections are taken",
                       Peer::DropDirection::WE_DROPPED_REMOTE);
            mInboundPeers.mDropped.insert(peer);
            return;
        }
        CLOG_DEBUG(Overlay, "New (inbound) connected peer {}",
                   peer->toString());
        mInboundPeers.mConnectionsEstablished.Mark();
        mInboundPeers.mPending.push_back(peer);
        updateSizeCounters();
    }
    else
    {
        mInboundPeers.mConnectionsCancelled.Mark();
    }
}

bool
OverlayManagerImpl::isPossiblyPreferred(std::string const& ip) const
{
    return std::any_of(
        std::begin(mConfigurationPreferredPeers),
        std::end(mConfigurationPreferredPeers),
        [&](PeerBareAddress const& address) { return address.getIP() == ip; });
}

bool
OverlayManagerImpl::haveSpaceForConnection(std::string const& ip) const
{
    auto totalAuthenticated = getInboundAuthenticatedPeers().size();
    auto totalTracked = *getLiveInboundPeersCounter();

    size_t totalPendingCount = 0;
    if (totalTracked > totalAuthenticated)
    {
        totalPendingCount = totalTracked - totalAuthenticated;
    }
    auto adjustedInCount =
        std::max<size_t>(mInboundPeers.mPending.size(), totalPendingCount);

    auto haveSpace =
        adjustedInCount < mApp.getConfig().MAX_INBOUND_PENDING_CONNECTIONS;

    if (!haveSpace &&
        adjustedInCount < mApp.getConfig().MAX_INBOUND_PENDING_CONNECTIONS +
                              Config::POSSIBLY_PREFERRED_EXTRA)
    {
        // for peers that are possibly preferred (they have the same IP as some
        // preferred peer we enocuntered in past), we allow an extra
        // Config::POSSIBLY_PREFERRED_EXTRA incoming pending connections, that
        // are not available for non-preferred peers
        haveSpace = isPossiblyPreferred(ip);
    }

    if (!haveSpace)
    {
        CLOG_DEBUG(
            Overlay,
            "Peer rejected - all pending inbound connections are taken: {}",
            ip);
        CLOG_DEBUG(Overlay, "If you wish to allow for more pending "
                            "inbound connections, please update your "
                            "MAX_PENDING_CONNECTIONS setting in "
                            "configuration file.");
    }

    return haveSpace;
}

bool
OverlayManagerImpl::addOutboundConnection(Peer::pointer peer)
{
    ZoneScoped;
    releaseAssert(peer->getRole() == Peer::WE_CALLED_REMOTE);
    mOutboundPeers.mConnectionsAttempted.Mark();

    if (!canAcceptOutboundPeer(peer->getAddress()))
    {
        mOutboundPeers.mConnectionsCancelled.Mark();
        peer->drop("all outbound connections taken",
                   Peer::DropDirection::WE_DROPPED_REMOTE);
        mOutboundPeers.mDropped.insert(peer);
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
    releaseAssert(threadIsMain());
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

std::shared_ptr<int>
OverlayManagerImpl::getLiveInboundPeersCounter() const
{
    return mLiveInboundPeersCounter;
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

    bool isPreferred = false;
    peer->doIfAuthenticated([&]() {
        isPreferred =
            mApp.getConfig().PREFERRED_PEER_KEYS.count(peer->getPeerID()) != 0;
    });

    if (isPreferred)
    {
        CLOG_DEBUG(Overlay, "Peer key {} is preferred",
                   mApp.getConfig().toShortString(peer->getPeerID()));
        return true;
    }

    CLOG_TRACE(Overlay, "Peer {} is not preferred", pstr);
    return false;
}

bool
OverlayManager::isFloodMessage(StellarMessage const& msg)
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
                                     Peer::pointer peer, Hash const& msgID)
{
    ZoneScoped;
    return mFloodGate.addRecord(msg, peer, msgID);
}

bool
OverlayManagerImpl::checkScheduledAndCache(
    std::shared_ptr<CapacityTrackedMessage> tracker)
{
#ifndef BUILD_TESTS
    releaseAssert(!threadIsMain() ||
                  !mApp.getConfig().BACKGROUND_OVERLAY_PROCESSING);
#endif
    if (!tracker->maybeGetHash())
    {
        return false;
    }
    auto index = tracker->maybeGetHash().value();
    if (mScheduledMessages.exists(index))
    {
        if (mScheduledMessages.get(index).lock())
        {
            return true;
        }
    }
    mScheduledMessages.put(index,
                           std::weak_ptr<CapacityTrackedMessage>(tracker));
    return false;
}

void
OverlayManagerImpl::recvTransaction(StellarMessage const& msg,
                                    Peer::pointer peer, Hash const& index)
{
    ZoneScoped;
    auto transaction = TransactionFrameBase::makeTransactionFromWire(
        mApp.getNetworkID(), msg.transaction());
    if (transaction)
    {
        // record that this peer sent us this transaction
        // add it to the floodmap so that this peer gets credit for it
        recvFloodedMsgID(msg, peer, index);

        mTxDemandsManager.recordTxPullLatency(transaction->getFullHash(), peer);

        // add it to our current set
        // and make sure it is valid
        auto addResult = mApp.getHerder().recvTransaction(transaction, false);
        bool pulledRelevantTx = false;
        if (!(addResult.code ==
                  TransactionQueue::AddResultCode::ADD_STATUS_PENDING ||
              addResult.code ==
                  TransactionQueue::AddResultCode::ADD_STATUS_DUPLICATE))
        {
            forgetFloodedMsg(index);
            CLOG_DEBUG(Overlay,
                       "Peer::recvTransaction Discarded transaction {} from {}",
                       hexAbbrev(transaction->getFullHash()), peer->toString());
        }
        else
        {
            bool dup = addResult.code ==
                       TransactionQueue::AddResultCode::ADD_STATUS_DUPLICATE;
            if (!dup)
            {
                pulledRelevantTx = true;
            }
            CLOG_DEBUG(
                Overlay,
                "Peer::recvTransaction Received {} transaction {} from {}",
                (dup ? "duplicate" : "unique"),
                hexAbbrev(transaction->getFullHash()), peer->toString());
        }

        auto const& om = getOverlayMetrics();
        auto& meter =
            pulledRelevantTx ? om.mPulledRelevantTxs : om.mPulledIrrelevantTxs;
        meter.Mark();
    }
}

void
OverlayManagerImpl::forgetFloodedMsg(Hash const& msgID)
{
    ZoneScoped;
    mFloodGate.forgetRecord(msgID);
}

void
OverlayManagerImpl::recvTxDemand(FloodDemand const& dmd, Peer::pointer peer)
{
    ZoneScoped;
    mTxDemandsManager.recvTxDemand(dmd, peer);
}

bool
OverlayManagerImpl::broadcastMessage(std::shared_ptr<StellarMessage const> msg,
                                     std::optional<Hash> const hash,
                                     uint32_t minOverlayVersion)
{
    ZoneScoped;
    auto res = mFloodGate.broadcast(msg, hash, minOverlayVersion);
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
    mDoor.close();
    mFloodGate.shutdown();
    mInboundPeers.shutdown();
    mOutboundPeers.shutdown();
    mTxDemandsManager.shutdown();

    // Switch overlay to "shutting down" state _after_ shutting down peers to
    // allow graceful connection drop
    mShuttingDown = true;

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
        stellarMsg.type() == SURVEY_RESPONSE ||
        stellarMsg.type() == TIME_SLICED_SURVEY_START_COLLECTING ||
        stellarMsg.type() == TIME_SLICED_SURVEY_STOP_COLLECTING ||
        stellarMsg.type() == TIME_SLICED_SURVEY_REQUEST ||
        stellarMsg.type() == TIME_SLICED_SURVEY_RESPONSE)
    {
        flood = true;
    }
    else if (stellarMsg.type() != TX_SET &&
             stellarMsg.type() != GENERALIZED_TX_SET &&
             stellarMsg.type() != SCP_QUORUMSET)
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

}
