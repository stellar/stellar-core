#pragma once

// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

/**
 * @class Tracker
 *
 * Asks peers for given data set. If a peer does not have given data set,
 * asks another one. If no peer does have given data set, it starts again
 * with new set of peers (possibly overlapping, as peers may learned about
 * this data set in meantime).
 *
 * For asking a AskPeer delegate is used.
 *
 * Tracker keeps list of envelopes that requires given data set to be
 * fully resolved. When data is received each envelope is resend to Herder
 * so it can check if it has all required data and then process envelope.
 * @see listen(Peer::pointer) is used to add envelopes to that list.
 */

#include "overlay/Peer.h"
#include "util/Timer.h"
#include "xdr/Stellar-types.h"

#include <functional>
#include <utility>
#include <vector>

namespace stellar
{

class Application;

using AskPeer = std::function<void(Peer::pointer, Hash)>;

class Tracker
{
  private:
    AskPeer mAskPeer;
    Application& mApp;
    Peer::pointer mLastAskedPeer;
    int mNumListRebuild;
    std::deque<Peer::pointer> mPeersToAsk;
    VirtualTimer mTimer;
    std::vector<std::pair<Hash, SCPEnvelope>> mWaitingEnvelopes;
    Hash mItemHash;
    medida::Meter& mTryNextPeer;
    uint64 mLastSeenSlotIndex{0};

  public:
    /**
     * Create Tracker that tracks data identified by @p hash. @p askPeer
     * delegate is used to fetch the data.
     */
    explicit Tracker(Application& app, Hash const& hash, AskPeer& askPeer);
    virtual ~Tracker();

    /**
     * Return true if does not wait for any envelope.
     */
    bool
    empty() const
    {
        return mWaitingEnvelopes.empty();
    }

    /**
     * Return list of envelopes this tracker is waiting for.
     */
    const std::vector<std::pair<Hash, SCPEnvelope>>&
    waitingEnvelopes() const
    {
        return mWaitingEnvelopes;
    }

    /**
     * Return count of envelopes it is waiting for.
     */
    size_t
    size() const
    {
        return mWaitingEnvelopes.size();
    }

    /**
     * Pop envelope from stack.
     */
    SCPEnvelope pop();

    /**
     * Called periodically to remove old envelopes from list (with ledger id
     * below some @p slotIndex).
     *
     * Returns true if at least one envelope remained in list.
     */
    bool clearEnvelopesBelow(uint64 slotIndex);

    /**
     * Add @p env to list of envelopes that will be resend to Herder when data
     * is received.
     */
    void listen(const SCPEnvelope& env);

    /**
     * Stops tracking envelope @p env.
     */
    void discard(const SCPEnvelope& env);

    /**
     * Stop the timer, stop requesting the item as we have it.
     */
    void cancel();

    /**
     * Called when given @p peer informs that it does not have given data.
     * Next peer will be tried if available.
     */
    void doesntHave(Peer::pointer peer);

    /**
     * Called either when @see doesntHave(Peer::pointer) was received or
     * request to peer timed out.
     */
    void tryNextPeer();

    /**
     * Return biggest slot index seen since last reset.
     */
    uint64
    getLastSeenSlotIndex() const
    {
        return mLastSeenSlotIndex;
    }

    /**
     * Reset value of biggest slot index seen.
     */
    void
    resetLastSeenSlotIndex()
    {
        mLastSeenSlotIndex = 0;
    }
};
}
