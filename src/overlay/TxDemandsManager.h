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
 * TxDemandsManager is responsible for managing transaction demand schedule, and
 * responding to demands.
 */

class TxDemandsManager : private NonMovableOrCopyable
{
  public:
    explicit TxDemandsManager(Application& app);

    // Record how long it took to pull a transaction
    void recordTxPullLatency(Hash const& hash, std::shared_ptr<Peer> peer);

    // Process demand from a peer, maybe send a transaction back
    void recvTxDemand(FloodDemand const& dmd, Peer::pointer peer);

    // Begin demanding transactions from peers
    void start();

    // Stop demanding transactions from peers
    void shutdown();

  private:
    // After `MAX_RETRY_COUNT` attempts with linear back-off, we assume that
    // no one has the transaction.
    static constexpr int MAX_RETRY_COUNT = 15;

    struct DemandHistory
    {
        VirtualClock::time_point firstDemanded;
        VirtualClock::time_point lastDemanded;
        UnorderedMap<NodeID, VirtualClock::time_point> peers;
        bool latencyRecorded{false};
    };
    enum class DemandStatus
    {
        DEMAND,      // Demand
        RETRY_LATER, // The timer hasn't expired, and we need to come back to
        // this.
        DISCARD // We should never demand this txn from this peer.
    };

    Application& mApp;
    VirtualTimer mDemandTimer;
    UnorderedMap<Hash, DemandHistory> mDemandHistoryMap;
    std::queue<Hash> mPendingDemands;

    // Begin demanding on schedule
    void startDemandTimer();

    // Construct demand messages based on adverts received from peers
    void demand();

    // Compute max number of transactions to demand based on network limits
    size_t getMaxDemandSize() const;

    // Decide whether to demand a transaction now, retry later or discard
    DemandStatus demandStatus(Hash const& txHash, Peer::pointer) const;

    // Compute delay between demand retries, with linear backoff
    std::chrono::milliseconds retryDelayDemand(int numAttemptsMade) const;
};
}
