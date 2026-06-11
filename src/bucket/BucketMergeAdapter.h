// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "bucket/BucketInputIterator.h"
#include "bucket/LedgerCmp.h"
#include "bucket/LiveBucket.h"
#include <optional>
#include <vector>

namespace stellar
{

// These classes provide wrappers around the inputs to a BucketMerge, namely
// either BucketInputIterators for file based merges, or a vector of bucket
// entries for in-memory merges.
template <class BucketT> class MergeInput
{
  public:
    // Check if we're done - both iterators are exhausted
    virtual bool isDone() const = 0;

    // Check if old entry should go first (old < new or new is exhausted)
    virtual bool oldFirst() const = 0;

    // Check if new entry should go first (new < old or old is exhausted)
    virtual bool newFirst() const = 0;

    // Check if old and new entries have equal keys and are both not exhausted
    virtual bool equalKeys() const = 0;

    virtual typename BucketT::EntryT const& getOldEntry() = 0;
    virtual typename BucketT::EntryT const& getNewEntry() = 0;

    // Advance iterators
    virtual void advanceOld() = 0;
    virtual void advanceNew() = 0;

    virtual ~MergeInput() = default;
};

template <class BucketT> class FileMergeInput : public MergeInput<BucketT>
{
  private:
    BucketInputIterator<BucketT>& mOldIter;
    BucketInputIterator<BucketT>& mNewIter;
    BucketEntryIdCmp<BucketT> mCmp;

  public:
    FileMergeInput(BucketInputIterator<BucketT>& oldIter,
                   BucketInputIterator<BucketT>& newIter)
        : mOldIter(oldIter), mNewIter(newIter)
    {
    }

    bool
    isDone() const override
    {
        return !mOldIter && !mNewIter;
    }

    bool
    oldFirst() const override
    {
        return !mNewIter || (mOldIter && mCmp(*mOldIter, *mNewIter));
    }

    bool
    newFirst() const override
    {
        return !mOldIter || (mNewIter && mCmp(*mNewIter, *mOldIter));
    }

    bool
    equalKeys() const override
    {
        return mOldIter && mNewIter && !mCmp(*mOldIter, *mNewIter) &&
               !mCmp(*mNewIter, *mOldIter);
    }

    typename BucketT::EntryT const&
    getOldEntry() override
    {
        return *mOldIter;
    }

    typename BucketT::EntryT const&
    getNewEntry() override
    {
        return *mNewIter;
    }

    void
    advanceOld() override
    {
        ++mOldIter;
    }

    void
    advanceNew() override
    {
        ++mNewIter;
    }
};

// Merge input where the "new" side is a composite (sharded) level-0 bucket.
// Presents the shard set as a single sorted stream by k-way walking the
// shards and folding same-key entries across shards (oldest to newest) with
// LiveBucket::mergeSameKeyEntries. Keys whose fold annihilates (INIT+DEAD)
// are skipped entirely, which matches the result of sequentially merging
// the shards pairwise.
class ShardedLiveMergeInput : public MergeInput<LiveBucket>
{
  private:
    // One cursor per shard, oldest shard first. Iterates the shard's
    // retained in-memory entries when present, falling back to file
    // iteration (e.g. for shards reconstituted from disk after a restart).
    struct ShardCursor
    {
        std::vector<BucketEntry> const* mEntries{nullptr};
        size_t mIdx{0};
        std::unique_ptr<BucketInputIterator<LiveBucket>> mIter;

        explicit ShardCursor(std::shared_ptr<LiveBucket> const& shard)
        {
            if (shard->hasInMemoryEntries())
            {
                mEntries = &shard->getInMemoryEntries();
            }
            else
            {
                mIter =
                    std::make_unique<BucketInputIterator<LiveBucket>>(shard);
            }
        }

        bool
        done() const
        {
            return mEntries ? mIdx >= mEntries->size() : !*mIter;
        }

        BucketEntry const&
        cur() const
        {
            return mEntries ? (*mEntries)[mIdx] : **mIter;
        }

        void
        advance()
        {
            if (mEntries)
            {
                ++mIdx;
            }
            else
            {
                ++*mIter;
            }
        }
    };

    BucketInputIterator<LiveBucket>& mOldIter;
    std::vector<ShardCursor> mCursors;
    std::optional<BucketEntry> mCurrentNew;
    BucketEntryIdCmp<LiveBucket> mCmp;
    MergeCounters& mMergeCounters;

    // Computes the next new-side entry: takes the minimal key across all
    // shard cursors, folds every same-key shard entry oldest-to-newest, and
    // skips keys that fold away to nothing.
    void
    settleNew()
    {
        mCurrentNew.reset();
        while (!mCurrentNew)
        {
            ShardCursor* minCursor = nullptr;
            for (auto& c : mCursors)
            {
                if (!c.done() &&
                    (minCursor == nullptr || mCmp(c.cur(), minCursor->cur())))
                {
                    minCursor = &c;
                }
            }
            if (minCursor == nullptr)
            {
                // All shards exhausted.
                return;
            }
            // Collect the cursors whose head matches the minimal key before
            // advancing any of them.
            std::vector<ShardCursor*> matches;
            for (auto& c : mCursors)
            {
                if (!c.done() && !mCmp(c.cur(), minCursor->cur()) &&
                    !mCmp(minCursor->cur(), c.cur()))
                {
                    matches.push_back(&c);
                }
            }
            std::optional<BucketEntry> folded;
            for (auto* c : matches)
            {
                if (folded)
                {
                    folded = LiveBucket::mergeSameKeyEntries(mMergeCounters,
                                                             *folded, c->cur());
                }
                else
                {
                    folded = c->cur();
                }
                c->advance();
            }
            // folded may be nullopt (annihilated); loop to the next key.
            mCurrentNew = std::move(folded);
        }
    }

  public:
    ShardedLiveMergeInput(BucketInputIterator<LiveBucket>& oldIter,
                          std::vector<std::shared_ptr<LiveBucket>> const& shards,
                          MergeCounters& mc)
        : mOldIter(oldIter), mMergeCounters(mc)
    {
        mCursors.reserve(shards.size());
        for (auto const& shard : shards)
        {
            mCursors.emplace_back(shard);
        }
        settleNew();
    }

    bool
    isDone() const override
    {
        return !mOldIter && !mCurrentNew;
    }

    bool
    oldFirst() const override
    {
        return !mCurrentNew || (mOldIter && mCmp(*mOldIter, *mCurrentNew));
    }

    bool
    newFirst() const override
    {
        return !mOldIter || (mCurrentNew && mCmp(*mCurrentNew, *mOldIter));
    }

    bool
    equalKeys() const override
    {
        return mOldIter && mCurrentNew && !mCmp(*mOldIter, *mCurrentNew) &&
               !mCmp(*mCurrentNew, *mOldIter);
    }

    BucketEntry const&
    getOldEntry() override
    {
        return *mOldIter;
    }

    BucketEntry const&
    getNewEntry() override
    {
        releaseAssertOrThrow(mCurrentNew);
        return *mCurrentNew;
    }

    void
    advanceOld() override
    {
        ++mOldIter;
    }

    void
    advanceNew() override
    {
        settleNew();
    }
};

template <class BucketT> class MemoryMergeInput : public MergeInput<BucketT>
{
  private:
    std::vector<typename BucketT::EntryT> const& mOldEntries;
    std::vector<typename BucketT::EntryT> const& mNewEntries;
    BucketEntryIdCmp<BucketT> mCmp;
    size_t mOldIdx = 0;
    size_t mNewIdx = 0;

  public:
    MemoryMergeInput(std::vector<typename BucketT::EntryT> const& oldEntries,
                     std::vector<typename BucketT::EntryT> const& newEntries)
        : mOldEntries(oldEntries), mNewEntries(newEntries)
    {
    }

    bool
    isDone() const override
    {
        return mOldIdx >= mOldEntries.size() && mNewIdx >= mNewEntries.size();
    }

    bool
    oldFirst() const override
    {
        return mNewIdx >= mNewEntries.size() ||
               (mOldIdx < mOldEntries.size() &&
                mCmp(mOldEntries.at(mOldIdx), mNewEntries.at(mNewIdx)));
    }

    bool
    newFirst() const override
    {
        return mOldIdx >= mOldEntries.size() ||
               (mNewIdx < mNewEntries.size() &&
                mCmp(mNewEntries.at(mNewIdx), mOldEntries.at(mOldIdx)));
    }

    bool
    equalKeys() const override
    {
        return mOldIdx < mOldEntries.size() && mNewIdx < mNewEntries.size() &&
               !mCmp(mOldEntries.at(mOldIdx), mNewEntries.at(mNewIdx)) &&
               !mCmp(mNewEntries.at(mNewIdx), mOldEntries.at(mOldIdx));
    }

    typename BucketT::EntryT const&
    getOldEntry() override
    {
        return mOldEntries.at(mOldIdx);
    }

    typename BucketT::EntryT const&
    getNewEntry() override
    {
        return mNewEntries.at(mNewIdx);
    }

    void
    advanceOld() override
    {
        ++mOldIdx;
    }

    void
    advanceNew() override
    {
        ++mNewIdx;
    }
};
}
