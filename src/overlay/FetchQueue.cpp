// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/FetchQueue.h"
#include "overlay/ItemKey.h"

namespace stellar
{

void
FetchQueue::push(VirtualClock::time_point when, ItemKey what)
{
    mCounts[what]++;
    mQueue.push(std::make_pair(when, what));
}

void
FetchQueue::pop()
{
    auto what = top().second;
    mQueue.pop();

    if (--mCounts[what] == 0)
    {
        mCounts.erase(what);
    }
}

void
FetchQueue::rescheduleTop(VirtualClock::time_point when)
{
    auto item = top().second;
    pop();
    push(when, item);
}

uint32_t
FetchQueue::count(ItemKey what) const
{
    auto it = mCounts.find(what);
    return it == std::end(mCounts) ? 0 : it->second;
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
