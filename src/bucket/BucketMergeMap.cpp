// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketMergeMap.h"
#include "crypto/Hex.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include <Tracy.hpp>

namespace
{
stellar::UnorderedSet<stellar::HashID>
getMergeKeyHashes(stellar::MergeKey const& key)
{
    ZoneScoped;
    stellar::UnorderedSet<stellar::HashID> hashes;
    hashes.emplace(key.mInputCurrBucket);
    hashes.emplace(key.mInputSnapBucket);
    for (auto const& in : key.mInputShadowBuckets)
    {
        hashes.emplace(in);
    }
    return hashes;
}
}

namespace stellar
{

void
BucketMergeMap::recordMerge(MergeKey const& input, HashID const& output)
{
    ZoneScoped;
    mMergeKeyToOutput.emplace(input, output);
    mOutputToMergeKey.emplace(output, input);
    for (auto const& in : getMergeKeyHashes(input))
    {
        CLOG_TRACE(Bucket, "BucketMergeMap retaining mapping for {} -> {}",
                   in.toLogString(), output.toLogString());
        mInputToOutput.emplace(in, output);
    }
}

UnorderedSet<MergeKey>
BucketMergeMap::forgetAllMergesProducing(HashID const& outputBeingDropped)
{
    ZoneScoped;
    UnorderedSet<MergeKey> ret;
    auto mergesProducingOutput =
        mOutputToMergeKey.equal_range(outputBeingDropped);
    for (auto mergeProducingOutput = mergesProducingOutput.first;
         mergeProducingOutput != mergesProducingOutput.second;
         mergeProducingOutput = mOutputToMergeKey.erase(mergeProducingOutput))
    {
        auto const& output = mergeProducingOutput->first;
        auto const& mergeKeyProducingOutput = mergeProducingOutput->second;
        releaseAssert(output == outputBeingDropped);
        ret.emplace(mergeKeyProducingOutput);

        // It's possible for the same output to occur for multiple
        // merge keys (eg. a+b and b+a). And the set of per-input
        // entries should always be larger than the per-key entries.
        if ((mOutputToMergeKey.size() > mMergeKeyToOutput.size()) ||
            (mMergeKeyToOutput.size() > mInputToOutput.size()))
        {
            CLOG_WARNING(Bucket,
                         "BucketMergeMap inconsistent map sizes: out->in:{}, "
                         "coarse in->out:{}, fine in->out:{}",
                         mOutputToMergeKey.size(), mMergeKeyToOutput.size(),
                         mInputToOutput.size());
        }

        CLOG_TRACE(
            Bucket,
            "BucketMergeMap forgetting mappings for merge {} <-> output={}",
            mergeKeyProducingOutput, outputBeingDropped.toLogString());

        // We first remove all the (in,out) pairs from the decomposed
        // mapping mInputToOutput, used for rooting the
        // publish queue.
        for (auto const& input : getMergeKeyHashes(mergeKeyProducingOutput))
        {
            auto mergesUsingInput = mInputToOutput.equal_range(input);
            for (auto mergeUsingInput = mergesUsingInput.first;
                 mergeUsingInput != mergesUsingInput.second; ++mergeUsingInput)
            {
                auto const& outputUsingInput = mergeUsingInput->second;
                if (outputUsingInput == outputBeingDropped)
                {
                    CLOG_TRACE(Bucket,
                               "BucketMergeMap forgetting mapping for {} -> {}",
                               input.toLogString(),
                               outputUsingInput.toLogString());
                    mInputToOutput.erase(mergeUsingInput);
                    break;
                }
            }
        }
        // Then we erase the forward mapping mergeKey => output;
        // the for-loop-step erases reverse mapping output => mergeKey.
        mMergeKeyToOutput.erase(mergeKeyProducingOutput);
    }
    return ret;
}

bool
BucketMergeMap::findMergeFor(MergeKey const& input, HashID& output)
{
    ZoneScoped;
    auto i = mMergeKeyToOutput.find(input);
    if (i != mMergeKeyToOutput.end())
    {
        output = i->second;
        return true;
    }
    return false;
}

void
BucketMergeMap::getOutputsUsingInput(HashID const& input,
                                     std::set<HashID>& outputs) const
{
    ZoneScoped;
    auto pair = mInputToOutput.equal_range(input);
    for (auto i = pair.first; i != pair.second; ++i)
    {
        outputs.emplace(i->second);
        CLOG_TRACE(Bucket, "{} referenced as output of merge of {}",
                   i->second.toLogString(), input.toLogString());
    }
}
}
