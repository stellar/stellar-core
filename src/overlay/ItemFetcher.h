// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "overlay/Peer.h"
#include "util/NonCopyable.h"
#include "util/Timer.h"
#include <functional>
#include <map>
#include <optional>

namespace medida
{
class Counter;
class Timer;
}

namespace stellar
{

class Tracker;
class TxSetXDRFrame;
struct SCPQuorumSet;
using SCPQuorumSetPtr = std::shared_ptr<SCPQuorumSet>;
using AskPeer = std::function<void(Peer::pointer, Hash)>;

/**
 * @class ItemFetcher
 *
 * Manages asking for Transaction or Quorum sets from Peers
 *
 * The ItemFetcher keeps instances of the Tracker class. There exists exactly
 * one Tracker per item. The tracker is used to maintain the state of the
 * search.
 */
class ItemFetcher : private NonMovableOrCopyable
{
  public:
    using TrackerPtr = std::shared_ptr<Tracker>;

    /**
     * Create ItemFetcher that fetches data using @p askPeer delegate.
     */
    explicit ItemFetcher(Application& app, AskPeer askPeer);

    /**
     * Fetch data identified by @p hash and needed by @p envelope. Multiple
     * envelopes may require one set of data.
     */
    void fetch(Hash const& itemHash, SCPEnvelope const& envelope);

    /**
     * Stops fetching data identified by @p hash for @p envelope. If other
     * envelopes requires this data, it is still being fetched, but
     * @p envelope will not be notified about it.
     */
    void stopFetch(Hash const& itemHash, SCPEnvelope const& envelope);

    /**
     * Return biggest slot index seen for given hash. If 0, then given hash
     * is not being fetched.
     */
    uint64 getLastSeenSlotIndex(Hash const& itemHash) const;

    /**
     * Return envelopes that require data identified by @p hash.
     */
    std::vector<SCPEnvelope> fetchingFor(Hash const& itemHash) const;

    /**
     * Return how long the fetcher has been waiting for the item identified by
     * @p hash. Returns nullopt if the item is not being fetched.
     */
    // TODO: Maybe update the name of this function and doc comment. I don't
    // like "waiting time" or "nulopt if the item is not being fetched".
    // Technically this returns the time since the fetch was started, but if the
    // fetch has completed it STILL returns the time since the fetch started,
    // and so it's not necessarily all "waiting time".
    std::optional<std::chrono::milliseconds>
    getWaitingTime(Hash const& itemHash) const;

    /**
     * Called periodically to remove envelopes from list that fall outside
     * the range [minSlot, maxSlot]. Either bound may be nullopt to skip
     * that direction. Can also remove @see Tracker instances when not
     * needed anymore.
     */
    void stopFetchingOutsideRange(std::optional<uint64> minSlot,
                                  std::optional<uint64> maxSlot,
                                  uint64 slotToKeep);

    /**
     * Called when given @p peer informs that it does not have data identified
     * by @p itemHash.
     */
    void doesntHave(Hash const& itemHash, Peer::pointer peer);

    /**
     * Called when data with given @p itemHash was received. All envelopes
     * added before with @see fetch and the same @p itemHash will be resent
     * to Herder, matching @see Tracker will be cleaned up.
     */
    void recv(Hash const& itemHash, medida::Timer& timer);

#ifdef BUILD_TESTS
    std::shared_ptr<Tracker> getTracker(Hash const& h);
#endif

  protected:
    void stopFetchingOutsideRangeInternal(std::optional<uint64> minSlot,
                                          std::optional<uint64> maxSlot,
                                          uint64 slotToKeep);

    Application& mApp;
    std::map<Hash, std::shared_ptr<Tracker>> mTrackers;

  private:
    AskPeer mAskPeer;
};
}
