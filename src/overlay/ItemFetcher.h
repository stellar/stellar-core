#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/Peer.h"
#include "util/HashOfHash.h"
#include "util/NonCopyable.h"
#include "util/Timer.h"
#include <deque>
#include <functional>
#include <map>
#include <util/optional.h>

namespace medida
{
class Counter;
class Timer;
}

namespace stellar
{

class Tracker;
class TxSetFrame;
struct SCPQuorumSet;
using TxSetFramePtr = std::shared_ptr<TxSetFrame>;
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
     * Called periodically to remove old envelopes from list (with ledger id
     * below some @p slotIndex). Can also remove @see Tracker instances when
     * non needed anymore.
     */
    void stopFetchingBelow(uint64 slotIndex);

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
    void recv(Hash itemHash, medida::Timer& timer);

#ifdef BUILD_TESTS
    std::shared_ptr<Tracker> getTracker(Hash const& h);
#endif

  protected:
    void stopFetchingBelowInternal(uint64 slotIndex);

    Application& mApp;
    std::map<Hash, std::shared_ptr<Tracker>> mTrackers;

  private:
    AskPeer mAskPeer;
};
}
