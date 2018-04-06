// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/FetchQueue.h"
#include "item/ItemKey.h"

namespace stellar
{

void
FetchQueue::push(VirtualClock::time_point when, ItemKey what)
{
    mQueue.push(std::make_pair(when, what));
}

void
FetchQueue::pop()
{
    mQueue.pop();
}

void
FetchQueue::pushToLater(VirtualClock::time_point when)
{
    auto item = top().second;
    pop();
    push(when, item);
}

bool
FetchQueue::empty() const
{
    return mQueue.empty();
}

FetchQueue::value_type const&
FetchQueue::top() const
{
    return mQueue.top();
}

bool
FetchQueue::Compare::operator()(value_type const& x, value_type const& y)
{
    return x.first > y.first;
}
}
