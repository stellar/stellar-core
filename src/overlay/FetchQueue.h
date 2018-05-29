#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Timer.h"
#include "xdr/Stellar-types.h"

#include <queue>
#include <utility>

namespace stellar
{

class ItemKey;

/**
 * Simple priority queue that returns elements with lowest time first. Used to
 * start fetching jobs in order.
 */
class FetchQueue
{
  public:
    using value_type = std::pair<VirtualClock::time_point, ItemKey>;

    /**
     * Add new fetching job at given time.
     */
    void push(VirtualClock::time_point when, ItemKey what);

    /**
     * Remove top job (with lowest time) from queue.
     */
    void pop();

    /**
     * Remove top job (with lowest time) from queue and then add it again at new
     * time.
     */
    void pushToLater(VirtualClock::time_point when);

    uint32_t count(ItemKey what) const;

    bool empty() const;

    /**
     * Return top job (with lowest time) from queue.
     */
    value_type const& top() const;

  private:
    struct Compare
    {
      public:
        bool operator()(value_type const& x, value_type const& y);
    };

    std::priority_queue<value_type, std::vector<value_type>, Compare> mQueue;
    std::map<ItemKey, uint32_t> mCounts;
};
}
