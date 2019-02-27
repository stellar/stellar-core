#pragma once
// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Math.h"

#include <random>
#include <unordered_map>

namespace stellar
{

// Implements a simple fixed-size cache that does worse-of-2-random-choices
// eviction. Degrades more-gracefully across pathological load patterns than
// LRU, and also takes somewhat less book-keeping.
template <typename K, typename V> class CacheTable
{
  public:
    struct Counters
    {
        uint64_t mHits{0};
        uint64_t mMisses{0};
        uint64_t mInserts{0};
        uint64_t mUpdates{0};
        uint64_t mEvicts{0};
    };

  private:
    // Cache will evict entries once it exceeds this size.
    size_t mMaxSize;

    // Entries are stored in a wrapper that includes time-of-last-access via a
    // simple generation counter that's incremented on each cache mutation.
    uint64_t mGeneration;
    struct CacheValue
    {
        uint64_t mLastAccess;
        V mValue;
    };

    // Cache itself is stored in a hashmap.
    std::unordered_map<K, CacheValue> mValueMap;
    using map_value_type =
        typename std::unordered_map<K, CacheValue>::value_type;

    // Pointers to hashmap elements are stored redundantly here just so we can
    // randomly pick some to evict; unordered_map has no random-access iterators
    // (most hashtables don't). Pointers to elements of unordered_map are stable
    // even across rehashing or deletion of other elements, by design.
    std::vector<map_value_type*> mValuePtrs;

    // Each cache keeps some counters just to monitor its performance.
    Counters mCounters;

    // Randomly pick two elements and evict the less-recently-used one.
    void
    evictOne()
    {
        size_t sz = mValuePtrs.size();
        if (sz == 0)
        {
            return;
        }
        map_value_type*& vp1 = mValuePtrs.at(rand_uniform<size_t>(0, sz - 1));
        map_value_type*& vp2 = mValuePtrs.at(rand_uniform<size_t>(0, sz - 1));
        map_value_type*& victim =
            (vp1->second.mLastAccess < vp2->second.mLastAccess ? vp1 : vp2);
        mValueMap.erase(victim->first);
        std::swap(victim, mValuePtrs.back());
        mValuePtrs.pop_back();
        ++mCounters.mEvicts;
    }

  public:
    CacheTable(size_t maxSize) : mMaxSize(maxSize)
    {
        mValueMap.reserve(maxSize);
        mValuePtrs.reserve(maxSize);
    }

    size_t
    maxSize() const
    {
        return maxSize;
    }

    size_t
    size() const
    {
        return mValueMap.size();
    }

    Counters const&
    getCounters() const
    {
        return mCounters;
    }

    void
    put(K const& k, V const& v)
    {
        ++mGeneration;
        CacheValue newValue{mGeneration, v};
        auto pair = mValueMap.insert(std::make_pair(k, newValue));
        if (pair.second)
        {
            // An insertion occurred: save a pointer to the pair inserted.
            map_value_type& inserted = *pair.first;
            mValuePtrs.push_back(&inserted);
            ++mCounters.mInserts;
            // We may have just grown over the size limit. Fix that.
            while (mValuePtrs.size() > mMaxSize)
            {
                evictOne();
            }
        }
        else
        {
            // No insertion happened, was already an entry: update its value.
            CacheValue& existing = pair.first->second;
            existing = newValue;
            ++mCounters.mUpdates;
        }
    }

    bool
    exists(K const& k, bool countMisses = true)
    {
        bool miss = (mValueMap.find(k) == mValueMap.end());
        // We do not count hits here; but we usually count misses, as exists()
        // is typically used as a guard followed by a get(). If you're using it
        // _not_ that way, pass `false` for countMisses.
        //
        // Or use the maybeGet interface, which will save you a second hash
        // lookup and provides a less-cumbersome interface.
        if (miss && countMisses)
        {
            ++mCounters.mMisses;
        }
        return !miss;
    }

    void
    clear()
    {
        mValuePtrs.clear();
        mValueMap.clear();
    }

    template <typename F>
    void
    erase_if(F const& f)
    {
        for (size_t i = 0; i < mValuePtrs.size(); ++i)
        {
            map_value_type*& vp = mValuePtrs.at(i);
            while (mValuePtrs.size() != 0 && f(vp->second.mValue))
            {
                mValueMap.erase(vp->first);
                std::swap(vp, mValuePtrs.back());
                mValuePtrs.pop_back();
            }
        }
    }

    V&
    get(K const& k)
    {
        if (auto* p = maybeGet(k))
        {
            return *p;
        }
        else
        {
            throw std::range_error("There is no such key in cache");
        }
    }

    V*
    maybeGet(K const& k)
    {
        auto it = mValueMap.find(k);
        if (it == mValueMap.end())
        {
            ++mCounters.mMisses;
            return nullptr;
        }
        else
        {
            ++mCounters.mHits;
            it->second.mLastAccess = ++mGeneration;
            return &it->second.mValue;
        }
    }
};
}
