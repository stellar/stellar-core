#pragma once

#include "bucket/MergeKey.h"
#include "util/HashOfHash.h"
#include "util/UnorderedMap.h"
#include "util/UnorderedSet.h"
#include "xdr/Stellar-types.h"
#include <set>

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

namespace stellar
{

// Helper type for BucketManager. Stores a bi-directional (weak, multi)mapping
// relating merge inputs and outputs, along with a decomposition of the map to a
// finer-grained one that associates each individual input in each MergeKey with
// all the outputs it's used by.
class BucketMergeMap
{
    // Records bucket input-output relationships for any buckets in that were
    // built from a merge during the lifetime of this BucketMergeMap (some were
    // not -- they just showed up from catchup or state reloading). Entries in
    // this map will be cleared when their _output_ is dropped from the owning
    // BucketManager's mSharedBuckets, typically due to being unreferenced.
    UnorderedMap<MergeKey, Hash> mMergeKeyToOutput;

    // Unfortunately to use this correctly in the bucket GC path, we also need
    // a different version of the same map, keyed by each input separately. And
    // since one input may be used in _many_ merges, it has to be a multimap.
    std::unordered_multimap<Hash, Hash> mInputToOutput;

    // Finally we need _another_ map, the opposite direction, to allow us to
    // erase entries from the previous two maps when we finally decide to drop
    // a bucket because nobody's going to reattach to it as far as we can tell.
    //
    // This also has to be a multimap because the same output can be produced
    // by multiple MergeKeys.
    std::unordered_multimap<Hash, MergeKey> mOutputToMergeKey;

  public:
    void recordMerge(MergeKey const& input, Hash const& output);
    UnorderedSet<MergeKey> forgetAllMergesProducing(Hash const& output);
    bool findMergeFor(MergeKey const& input, Hash& output);
    void getOutputsUsingInput(Hash const& input, std::set<Hash>& outputs) const;
};
}
