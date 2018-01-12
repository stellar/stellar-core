#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/StellarXDR.h"
#include <cereal/cereal.hpp>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace stellar
{

class Bucket;
class Application;

/**
 * FutureBucket is a minor wrapper around
 * std::shared_future<std::shared_ptr<Bucket>>, used in merging multiple buckets
 * together in the BucketList. The reason this is a separate class is that we
 * need to support a level of persistence: serializing merges-in-progress in a
 * symbolic fashion, including restarting the merges after we deserialize.
 *
 * This class is therefore used not _only_ in the BucketList but also in places
 * that serialize and deserialize snapshots of it in the form of
 * HistoryArchiveStates: the LedgerManager, when storing kHistoryArchiveState at
 * the bottom of closeLedger; and the HistoryManager, when storing and
 * retrieving HistoryArchiveStates.
 */
class FutureBucket
{
    // There are two lifecycles of a FutureBucket:
    //
    // In one, it's created live, snapshotted at some point in the process
    // (either before or after the merge completes), and resolved/cleared.
    //
    // In another, it's created, deserialized (filling in the hashes of either
    // the inputs or the output, but not both), made-live, and resolved/cleared.

    enum State
    {
        FB_CLEAR = 0,       // No inputs; no outputs; no hashes.
        FB_HASH_OUTPUT = 1, // Output hash; no output; no inputs or hashes.
        FB_HASH_INPUTS = 2, // Input hashes; no inputs; no outputs or hashes.
        FB_LIVE_OUTPUT = 3, // Output; output hashes; _maybe_ inputs and hashes.
        FB_LIVE_INPUTS = 4, // Inputs; input hashes; no outputs. Merge running.
    };

    State mState{FB_CLEAR};

    // These live values hold the input buckets and/or output std::shared_future
    // for a "live" bucket merge in progress. They will be empty when the
    // FutureBucket is constructed, when it is reset, or when it is freshly
    // deserialized and not yet activated. When they are nonempty, they should
    // have values equal to the subsequent mFooHash values below.
    std::shared_ptr<Bucket> mInputCurrBucket;
    std::shared_ptr<Bucket> mInputSnapBucket;
    std::vector<std::shared_ptr<Bucket>> mInputShadowBuckets;
    std::shared_future<std::shared_ptr<Bucket>> mOutputBucket;

    // These strings hold the serializable (or deserialized) bucket hashes of
    // the inputs and outputs of a merge; depending on the state of the
    // FutureBucket they may be empty strings, but if they are nonempty and the
    // live values (mInputSnapBucket etc.) are also nonempty, they should agree
    // on the hash-values.
    std::string mInputCurrBucketHash;
    std::string mInputSnapBucketHash;
    std::vector<std::string> mInputShadowBucketHashes;
    std::string mOutputBucketHash;

    void checkHashesMatch() const;
    void checkState() const;
    void startMerge(Application& app, bool keepDeadEntries);

    void clearInputs();
    void clearOutput();
    void setLiveOutput(std::shared_ptr<Bucket> b);

  public:
    FutureBucket(Application& app, std::shared_ptr<Bucket> const& curr,
                 std::shared_ptr<Bucket> const& snap,
                 std::vector<std::shared_ptr<Bucket>> const& shadows,
                 bool keepDeadEntries);

    FutureBucket() = default;
    FutureBucket(FutureBucket const& other) = default;
    FutureBucket& operator=(FutureBucket const& other) = default;

    // Clear all live values and hashes.
    void clear();

    // Returns whether this object is in a FB_LIVE_FOO state.
    bool isLive() const;

    // Returns whether this object is in a FB_LIVE_INPUTS state (a merge is
    // running).
    bool isMerging() const;

    // Returns whether this object is in a FB_HASH_FOO state.
    bool hasHashes() const;

    // Returns whether this object is in FB_HASH_OUTPUT state.
    bool hasOutputHash() const;

    // Precondition: hasOutputHash(); return the hash.
    std::string const& getOutputHash() const;

    // Precondition: isLive(); returns whether a live merge is ready to resolve.
    bool mergeComplete() const;

    // Precondition: isLive(); waits-for and resolves to merged bucket.
    std::shared_ptr<Bucket> resolve();

    // Precondition: !isLive(); transitions from FB_HASH_FOO to FB_LIVE_FOO
    void makeLive(Application& app, bool keepDeadEntries);

    // Return all hashes referenced by this future.
    std::vector<std::string> getHashes() const;

    template <class Archive>
    void
    load(Archive& ar)
    {
        clear();
        ar(cereal::make_nvp("state", mState));
        switch (mState)
        {
        case FB_HASH_INPUTS:
            ar(cereal::make_nvp("curr", mInputCurrBucketHash));
            ar(cereal::make_nvp("snap", mInputSnapBucketHash));
            ar(cereal::make_nvp("shadow", mInputShadowBucketHashes));
            break;
        case FB_HASH_OUTPUT:
            ar(cereal::make_nvp("output", mOutputBucketHash));
            break;
        case FB_CLEAR:
            break;
        default:
            throw std::runtime_error(
                "deserialized unexpected FutureBucket state");
            break;
        }
        checkState();
    }

    template <class Archive>
    void
    save(Archive& ar) const
    {
        checkState();
        switch (mState)
        {
        case FB_LIVE_INPUTS:
        case FB_HASH_INPUTS:
            ar(cereal::make_nvp("state", FB_HASH_INPUTS));
            ar(cereal::make_nvp("curr", mInputCurrBucketHash));
            ar(cereal::make_nvp("snap", mInputSnapBucketHash));
            ar(cereal::make_nvp("shadow", mInputShadowBucketHashes));
            break;
        case FB_LIVE_OUTPUT:
        case FB_HASH_OUTPUT:
            ar(cereal::make_nvp("state", FB_HASH_OUTPUT));
            ar(cereal::make_nvp("output", mOutputBucketHash));
            break;
        case FB_CLEAR:
            ar(cereal::make_nvp("state", FB_CLEAR));
            break;
        default:
            assert(false);
            break;
        }
    }
};
}
