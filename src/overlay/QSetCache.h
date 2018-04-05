#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/HashOfHash.h"
#include "xdr/Stellar-SCP.h"
#include "xdr/Stellar-types.h"

#include <lib/util/lrucache.hpp>

namespace stellar
{

using SCPQuorumSetPtr = std::shared_ptr<SCPQuorumSet>;

// all the quorum sets we have learned about
class QSetCache
{
  public:
    QSetCache();

    void add(Hash hash, const SCPQuorumSet& qset);
    void touch(Hash hash);

    bool contains(Hash hash) const;
    SCPQuorumSetPtr get(Hash const& hash);

  private:
    cache::lru_cache<Hash, SCPQuorumSetPtr> mQsetCache;
};
}
