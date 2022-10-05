#pragma once

// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Application.h"

namespace stellar
{

class Peer;

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

    std::deque<Hash> mIncomingTxHashes;
    std::list<Hash> mTxHashesToRetry;

  public:
    TxAdvertQueue(Application& app);

    size_t size() const;

    Hash pop();

    void queueAndMaybeTrim(TxAdvertVector const& hash,
                           std::shared_ptr<Peer> peer);
    void appendHashesToRetryAndMaybeTrim(std::list<Hash>& list,
                                         std::shared_ptr<Peer> peer);
};
}
