#pragma once

// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/Peer.h"
#include "util/NonCopyable.h"

namespace medida
{
class Counter;
class Timer;
}

namespace stellar
{

/**
 * @class TxFloodManager
 *
 * Manages queuing incoming and outgoing tx hashes including demands
 *
 */

class TxFloodManager : private NonMovableOrCopyable
{
  public:
    explicit TxFloodManager(Application& app);
    static std::unique_ptr<TxFloodManager> create(Application& app);

    void recordTxPullLatency(Hash const& hash, std::shared_ptr<Peer> peer);

    void start();
    void shutdown();

  private:
    Application& mApp;

    void startDemandTimer();
    void demand();
    VirtualTimer mDemandTimer;

    size_t getMaxDemandSize() const;

    struct DemandHistory
    {
        VirtualClock::time_point firstDemanded;
        VirtualClock::time_point lastDemanded;
        UnorderedMap<NodeID, VirtualClock::time_point> peers;
        bool latencyRecorded{false};
    };
    UnorderedMap<Hash, DemandHistory> mDemandHistoryMap;

    std::queue<Hash> mPendingDemands;
    enum class DemandStatus
    {
        DEMAND,      // Demand
        RETRY_LATER, // The timer hasn't expired, and we need to come back to
        // this.
        DISCARD // We should never demand this txn from this peer.
    };
    DemandStatus demandStatus(Hash const& txHash, Peer::pointer) const;

    // After `MAX_RETRY_COUNT` attempts with linear back-off, we assume that
    // no one has the transaction.
    int const MAX_RETRY_COUNT = 15;
    std::chrono::milliseconds retryDelayDemand(int numAttemptsMade) const;
};
}
