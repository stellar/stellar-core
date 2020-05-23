// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/RandomPeerSource.h"
#include "crypto/Random.h"
#include "database/Database.h"
#include "main/Application.h"
#include "overlay/StellarXDR.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/must_use.h"

#include <algorithm>
#include <cmath>
#include <fmt/format.h>
#include <regex>
#include <soci.h>
#include <vector>

namespace stellar
{

using namespace soci;
PeerQuery
RandomPeerSource::maxFailures(int maxFailures, bool requireOutobund)
{
    return {false, maxFailures,
            requireOutobund ? PeerTypeFilter::ANY_OUTBOUND
                            : PeerTypeFilter::INBOUND_ONLY};
}

namespace
{
PeerTypeFilter
peerTypeToFilter(PeerType peerType)
{
    switch (peerType)
    {
    case PeerType::INBOUND:
    {
        return PeerTypeFilter::INBOUND_ONLY;
    }
    case PeerType::OUTBOUND:
    {
        return PeerTypeFilter::OUTBOUND_ONLY;
    }
    case PeerType::PREFERRED:
    {
        return PeerTypeFilter::PREFERRED_ONLY;
    }
    default:
    {
        abort();
    }
    }
}
}

PeerQuery
RandomPeerSource::nextAttemptCutoff(PeerType requireExactType)
{
    return {true, Config::REALLY_DEAD_NUM_FAILURES_CUTOFF,
            peerTypeToFilter(requireExactType)};
}

RandomPeerSource::RandomPeerSource(PeerManager& peerManager,
                                   PeerQuery peerQuery)
    : mPeerManager(peerManager), mPeerQuery(std::move(peerQuery))
{
}

std::vector<PeerBareAddress>
RandomPeerSource::getRandomPeers(
    int size, std::function<bool(PeerBareAddress const&)> pred)
{
    assert(size >= 0);
    if (size == 0)
    {
        return {};
    }

    if (mPeerCache.size() < size)
    {
        mPeerCache = mPeerManager.loadRandomPeers(mPeerQuery, size);
    }

    auto result = std::vector<PeerBareAddress>{};
    auto it = std::begin(mPeerCache);
    auto end = std::end(mPeerCache);
    for (; it != end && result.size() < size; it++)
    {
        if (pred(*it))
        {
            result.push_back(*it);
        }
    }

    mPeerCache.erase(std::begin(mPeerCache), it);
    return result;
}
}
