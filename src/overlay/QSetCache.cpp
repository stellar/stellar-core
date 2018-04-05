// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/QSetCache.h"
#include "crypto/Hex.h"
#include "scp/QuorumSetUtils.h"
#include "util/Logging.h"

#define QSET_CACHE_SIZE 10000

namespace stellar
{

QSetCache::QSetCache() : mQsetCache{QSET_CACHE_SIZE}
{
}

void
QSetCache::add(Hash hash, const SCPQuorumSet& q)
{
    assert(isQuorumSetSane(q, false));

    CLOG(TRACE, "Herder") << "Add SCPQSet " << hexAbbrev(hash);

    mQsetCache.put(hash, std::make_shared<SCPQuorumSet>(q));
}

void
QSetCache::touch(Hash hash)
{
    get(hash);
}

bool
QSetCache::contains(Hash hash) const
{
    return mQsetCache.exists(hash);
}

SCPQuorumSetPtr
QSetCache::get(Hash const& hash)
{
    if (mQsetCache.exists(hash))
    {
        return mQsetCache.get(hash);
    }

    return SCPQuorumSetPtr{};
}
}
