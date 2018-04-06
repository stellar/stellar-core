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

class FetchQueue
{
  public:
    using value_type = std::pair<VirtualClock::time_point, ItemKey>;

    void push(VirtualClock::time_point when, ItemKey what);
    void pop();
    void pushToLater(VirtualClock::time_point when);

    bool empty() const;
    value_type const& top() const;

  private:
    struct Compare
    {
      public:
        bool operator()(value_type const& x, value_type const& y);
    };

    std::priority_queue<value_type, std::vector<value_type>, Compare> mQueue;
};
}
