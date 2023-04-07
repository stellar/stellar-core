#pragma once

// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Application.h"
#include "util/RandomEvictionCache.h"
#include "util/HashOfHash.h"

namespace stellar
{

// TxAdvertQueue class stores and properly trims incoming tx hashes
// and also maintains retries.
//
//
// When looking for the next tx hash to try,
// we first check the first element in the retry queue. If the retry
// queue is empty, then we look at mIncomingTxHashes and pop the first element.
// Both mIncomingTxHashes and mTxHashesToRetry are FIFO.

class TxAdvertQueue
{
  private:
    Application& mApp;

    std::deque<std::pair<Hash, VirtualClock::time_point>> mIncomingTxHashes;
    std::list<Hash> mTxHashesToRetry;

    // Cache seen hashes for a bit to avoid re-broadcasting the same data
    // transaction hash -> ledger number
    RandomEvictionCache<Hash, uint32_t> mAdvertHistory;

    void rememberHash(Hash const& hash, uint32_t ledgerSeq);

  public:
    TxAdvertQueue(Application& app);

    size_t size() const;

    std::pair<Hash, std::optional<VirtualClock::time_point>> pop();

    void queueAndMaybeTrim(TxAdvertVector const& hash, uint32_t seq);
    void appendHashesToRetryAndMaybeTrim(std::list<Hash>& list);
    bool peerKnowsHash(Hash const& hash);
    void clearBelow(uint32_t ledgerSeq);
};
}
