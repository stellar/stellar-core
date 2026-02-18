// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "bucket/BucketInputIterator.h"
#include "bucket/LedgerCmp.h"
#include <vector>

namespace stellar
{

// These classes provide wrappers around the inputs to a BucketMerge, namely
// either BucketInputIterators for file based merges, or a vector of bucket
// entries for in-memory merges.
template <IsBucketType BucketT> class MergeInput
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

template <IsBucketType BucketT>
class FileMergeInput : public MergeInput<BucketT>
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

template <IsBucketType BucketT>
class MemoryMergeInput : public MergeInput<BucketT>
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
