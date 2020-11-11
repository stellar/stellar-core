// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Scheduler.h"
#include "lib/util/finally.h"
#include "util/Timer.h"
#include <Tracy.hpp>
#include <cassert>

namespace stellar
{
using nsecs = std::chrono::nanoseconds;

class Scheduler::ActionQueue
    : public std::enable_shared_from_this<Scheduler::ActionQueue>
{
    struct Element
    {
        Action mAction;
        VirtualClock::time_point mEnqueueTime;
        Element(VirtualClock& clock, Action&& action)
            : mAction(std::move(action)), mEnqueueTime(clock.now())
        {
        }
    };

    std::string mName;
    ActionType mType;
    nsecs mTotalService{0};
    std::chrono::steady_clock::time_point mLastService;
    std::deque<Element> mActions;

    // mIdleList is a reference to the mIdleList member of the Scheduler that
    // owns this ActionQueue. mIdlePosition is an iterator to the position in
    // that list that this ActionQueue occupies, or mIdleList.end() if it's
    // not in mIdleList.
    std::list<Qptr>& mIdleList;
    std::list<Qptr>::iterator mIdlePosition;

  public:
    ActionQueue(std::string const& name, ActionType type,
                std::list<Qptr>& idleList)
        : mName(name)
        , mType(type)
        , mLastService(std::chrono::steady_clock::time_point::max())
        , mIdleList(idleList)
        , mIdlePosition(mIdleList.end())
    {
    }

    bool
    isInIdleList() const
    {
        return mIdlePosition != mIdleList.end();
    }

    void
    addToIdleList()
    {
        assert(!isInIdleList());
        assert(isEmpty());
        mIdleList.push_front(shared_from_this());
        mIdlePosition = mIdleList.begin();
    }

    void
    removeFromIdleList()
    {
        assert(isInIdleList());
        assert(isEmpty());
        mIdleList.erase(mIdlePosition);
        mIdlePosition = mIdleList.end();
    }

    std::string const&
    name() const
    {
        return mName;
    }

    ActionType
    type() const
    {
        return mType;
    }

    nsecs
    totalService() const
    {
        return mTotalService;
    }

    std::chrono::steady_clock::time_point
    lastService() const
    {
        return mLastService;
    }

    size_t
    size() const
    {
        return mActions.size();
    }

    bool
    isEmpty() const
    {
        return mActions.empty();
    }

    bool
    isOverloaded(nsecs latencyWindow, VirtualClock::time_point now) const
    {
        if (!mActions.empty())
        {
            auto timeInQueue = now - mActions.front().mEnqueueTime;
            return timeInQueue > latencyWindow;
        }
        return false;
    }

    size_t
    tryTrim(nsecs latencyWindow, VirtualClock::time_point now)
    {
        size_t n = 0;
        while (mType == ActionType::DROPPABLE_ACTION && !mActions.empty() &&
               isOverloaded(latencyWindow, now))
        {
            mActions.pop_front();
            n++;
        }
        return n;
    }

    void
    enqueue(VirtualClock& clock, Action&& action)
    {
        auto elt = Element(clock, std::move(action));
        mActions.emplace_back(std::move(elt));
    }

    void
    runNext(VirtualClock& clock, nsecs minTotalService)
    {
        ZoneScoped;
        ZoneText(mName.c_str(), mName.size());
        auto before = clock.now();
        Action action = std::move(mActions.front().mAction);
        mActions.pop_front();

        auto fini = gsl::finally([&]() {
            auto after = clock.now();
            nsecs duration = std::chrono::duration_cast<nsecs>(after - before);
            mTotalService = std::max(mTotalService + duration, minTotalService);
            mLastService = after;
        });

        action();
    }
};

Scheduler::Scheduler(VirtualClock& clock,
                     std::chrono::nanoseconds latencyWindow)
    : mRunnableActionQueues([](Qptr a, Qptr b) -> bool {
        return a->totalService() > b->totalService();
    })
    , mClock(clock)
    , mLatencyWindow(latencyWindow)
{
    setOverloaded(false);
}

void
Scheduler::trimSingleActionQueue(Qptr q, VirtualClock::time_point now)
{
    size_t trimmed = q->tryTrim(mLatencyWindow, now);
    mStats.mActionsDroppedDueToOverload += trimmed;
    mSize -= trimmed;
}

void
Scheduler::trimIdleActionQueues(VirtualClock::time_point now)
{
    if (mIdleActionQueues.empty())
    {
        return;
    }
    Qptr old = mIdleActionQueues.back();
    if (old->lastService() + mLatencyWindow < now)
    {
        assert(old->isEmpty());
        mAllActionQueues.erase(std::make_pair(old->name(), old->type()));
        old->removeFromIdleList();
    }
}

void
Scheduler::setOverloaded(bool overloaded)
{
    if (overloaded)
    {
        mOverloadedStart = mClock.now();
    }
    else
    {
        mOverloadedStart = std::chrono::steady_clock::time_point::max();
    }
}

void
Scheduler::enqueue(std::string&& name, Action&& action, ActionType type)
{
    auto key = std::make_pair(name, type);
    auto qi = mAllActionQueues.find(key);
    if (qi == mAllActionQueues.end())
    {
        mStats.mQueuesActivatedFromFresh++;
        auto q = std::make_shared<ActionQueue>(name, type, mIdleActionQueues);
        qi = mAllActionQueues.emplace(key, q).first;
        mRunnableActionQueues.push(qi->second);
    }
    else
    {
        if (qi->second->isInIdleList())
        {
            assert(qi->second->isEmpty());
            mStats.mQueuesActivatedFromIdle++;
            qi->second->removeFromIdleList();
            mRunnableActionQueues.push(qi->second);
        }
    }
    mStats.mActionsEnqueued++;
    qi->second->enqueue(mClock, std::move(action));
    mSize += 1;
}

size_t
Scheduler::runOne()
{
    auto start = mClock.now();
    trimIdleActionQueues(start);
    if (mRunnableActionQueues.empty())
    {
        assert(mSize == 0);
        return 0;
    }
    else
    {
        auto q = mRunnableActionQueues.top();
        mRunnableActionQueues.pop();
        trimSingleActionQueue(q, start);

        auto putQueueBackInIdleOrActive = gsl::finally([&]() {
            auto now = mClock.now();
            if (q->isOverloaded(mLatencyWindow, now))
            {
                if (mOverloadedStart ==
                    std::chrono::steady_clock::time_point::max())
                {
                    setOverloaded(true);
                }
            }
            else if (mOverloadedStart <
                     std::chrono::steady_clock::time_point::max())
            {
                // see if we're not overloaded anymore
                bool overloaded = std::any_of(
                    mAllActionQueues.begin(), mAllActionQueues.end(),
                    [&](std::pair<std::pair<std::string, ActionType>,
                                  Qptr> const& qp) {
                        return qp.second->isOverloaded(mLatencyWindow, now);
                    });
                if (!overloaded)
                {
                    setOverloaded(false);
                }
            }
            if (q->isEmpty())
            {
                mStats.mQueuesSuspended++;
                q->addToIdleList();
            }
            else
            {
                mRunnableActionQueues.push(q);
            }
        });

        if (!q->isEmpty())
        {
            // We pass along a "minimum service time" floor that the service
            // time of the queue will be incremented to, at minimum.
            auto minTotalService = mMaxTotalService - mLatencyWindow;
            mSize -= 1;
            mStats.mActionsDequeued++;
            auto updateMaxTotalService = gsl::finally([&]() {
                mMaxTotalService =
                    std::max(q->totalService(), mMaxTotalService);
                mCurrentActionType = ActionType::NORMAL_ACTION;
            });
            mCurrentActionType = q->type();
            q->runNext(mClock, minTotalService);
        }
        return 1;
    }
}

std::chrono::seconds
Scheduler::getOverloadedDuration() const
{
    auto now = mClock.now();
    std::chrono::seconds res;
    if (now > mOverloadedStart)
    {
        // round up
        res = std::chrono::duration_cast<std::chrono::seconds>(
                  now - mOverloadedStart) +
              std::chrono::seconds{1};
    }
    else
    {
        res = std::chrono::seconds{0};
    }
    return res;
}

Scheduler::ActionType
Scheduler::currentActionType() const
{
    return mCurrentActionType;
}

#ifdef BUILD_TESTS
std::shared_ptr<Scheduler::ActionQueue>
Scheduler::getExistingQueue(std::string const& name, ActionType type) const
{
    auto qi = mAllActionQueues.find(std::make_pair(name, type));
    if (qi == mAllActionQueues.end())
    {
        return nullptr;
    }
    return qi->second;
}

std::string const&
Scheduler::nextQueueToRun() const
{
    static std::string empty;
    if (mRunnableActionQueues.empty())
    {
        return empty;
    }
    return mRunnableActionQueues.top()->name();
}
std::chrono::nanoseconds
Scheduler::totalService(std::string const& q, ActionType type) const
{
    auto eq = getExistingQueue(q, type);
    assert(eq);
    return eq->totalService();
}

size_t
Scheduler::queueLength(std::string const& q, ActionType type) const
{
    auto eq = getExistingQueue(q, type);
    assert(eq);
    return eq->size();
}
#endif
}
