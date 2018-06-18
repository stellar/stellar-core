// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/ReadyEnvelopes.h"
#include "herder/Herder.h"
#include "herder/HerderUtils.h"
#include "main/Application.h"
#include "scp/SCP.h"
#include "util/Logging.h"

#include <medida/counter.h>
#include <medida/metrics_registry.h>

namespace stellar
{

ReadyEnvelopes::ReadyEnvelopes(Application& app)
    : mApp(app)
    , mReadyEnvelopesSize(
          mApp.getMetrics().NewCounter({"herder", "envelopes", "ready-size"}))
    , mSeenEnvelopesSize(
          mApp.getMetrics().NewCounter({"herder", "envelopes", "seen-size"}))
{
}

ReadyEnvelopes::~ReadyEnvelopes()
{
}

bool
ReadyEnvelopes::seen(SCPEnvelope const& envelope)
{
    auto slotIndex = envelope.statement.slotIndex;
    if (slotIndex < mMinimumSlotIndex)
    {
        return true;
    }

    auto& seen = mEnvelopes[slotIndex].mSeenEnvelopes;
    return seen.find(envelope) != std::end(seen);
}

bool
ReadyEnvelopes::push(SCPEnvelope const& envelope)
{
    if (seen(envelope))
    {
        return false;
    }

    traceEnvelope(mApp, "Marked ready", envelope);

    auto slotIndex = envelope.statement.slotIndex;
    assert(slotIndex >= mMinimumSlotIndex);
    assert(mEnvelopes[slotIndex].mSeenEnvelopes.find(envelope) ==
           std::end(mEnvelopes[slotIndex].mSeenEnvelopes));

    mEnvelopes[slotIndex].mSeenEnvelopes.insert(envelope);
    mEnvelopes[slotIndex].mReadyEnvelopes.push_back(envelope);

    mReadyEnvelopesSize.inc();
    mSeenEnvelopesSize.inc();
    return true;
}

bool
ReadyEnvelopes::pop(uint64_t slotIndex, SCPEnvelope& ret)
{
    auto it = mEnvelopes.begin();
    while (it != mEnvelopes.end() && it->first <= slotIndex)
    {
        auto& v = it->second.mReadyEnvelopes;
        if (v.size() != 0)
        {
            ret = v.back();
            v.pop_back();

            mReadyEnvelopesSize.dec();
            traceEnvelope(mApp, "Popped", ret);
            return true;
        }
        it++;
    }
    return false;
}

std::vector<uint64_t>
ReadyEnvelopes::readySlots()
{
    std::vector<uint64_t> result;
    for (auto const& entry : mEnvelopes)
    {
        if (!entry.second.mReadyEnvelopes.empty())
        {
            result.push_back(entry.first);
        }
    }
    return result;
}

void
ReadyEnvelopes::setMinimumSlotIndex(uint64_t slotIndex)
{
    mMinimumSlotIndex = slotIndex;

    for (auto iter = mEnvelopes.begin(); iter != mEnvelopes.end();)
    {
        if (iter->first < mMinimumSlotIndex)
        {
            mReadyEnvelopesSize.inc(iter->second.mReadyEnvelopes.size());
            mSeenEnvelopesSize.inc(iter->second.mSeenEnvelopes.size());

            iter = mEnvelopes.erase(iter);
        }
        else
            break;
    }
}

void
ReadyEnvelopes::dumpInfo(Json::Value& ret, size_t limit)
{
    auto& q = ret["queue"];
    auto it = mEnvelopes.rbegin();
    auto l = limit;
    while (it != mEnvelopes.rend() && l-- != 0)
    {
        if (!it->second.mReadyEnvelopes.empty() ||
            !it->second.mSeenEnvelopes.empty())
        {
            auto& i = q[std::to_string(it->first)];
            dumpEnvelopes(mApp.getHerder().getSCP(), i,
                          it->second.mReadyEnvelopes, "ready");
            dumpEnvelopes(mApp.getHerder().getSCP(), i,
                          it->second.mSeenEnvelopes, "seen");
        }
        it++;
    }
}
}
