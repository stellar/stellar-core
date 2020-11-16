#pragma once
// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Math.h"
#include "util/NonCopyable.h"

#include <random>
#include <unordered_map>

namespace stellar
{

// Implements a simple fixed-size cache that does
// least-recent-out-of-2-random-choices eviction. Degrades more-gracefully
// across pathological load patterns than LRU, and also takes somewhat less
// book-keeping.
template <typename K, typename V, typename Hash = std::hash<K>>
class RandomEvictionCache : public NonMovableOrCopyable
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
    uint64_t mGeneration{0};
    struct CacheValue
    {
        uint64_t mLastAccess;
        V mValue;
    };

    // Cache itself is stored in a hashmap.
    using MapType = std::unordered_map<K, CacheValue, Hash>;
    using MapValueType = typename MapType::value_type;
    MapType mValueMap;

    // Pointers to hashmap elements are stored redundantly here just so we can
    // randomly pick some to evict; unordered_map has no random-access iterators
    // (most hashtables don't). Pointers to elements of unordered_map are stable
    // even across rehashing or deletion of other elements, by design.
    std::vector<MapValueType*> mValuePtrs;

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
        MapValueType*& vp1 = mValuePtrs.at(rand_uniform<size_t>(0, sz - 1));
        MapValueType*& vp2 = mValuePtrs.at(rand_uniform<size_t>(0, sz - 1));
        MapValueType*& victim =
            (vp1->second.mLastAccess < vp2->second.mLastAccess ? vp1 : vp2);
        mValueMap.erase(victim->first);
        std::swap(victim, mValuePtrs.back());
        mValuePtrs.pop_back();
        ++mCounters.mEvicts;
    }

  public:
    explicit RandomEvictionCache(size_t maxSize) : mMaxSize(maxSize)
    {
        mValueMap.reserve(maxSize + 1);
        mValuePtrs.reserve(maxSize + 1);
    }

    size_t
    maxSize() const
    {
        return mMaxSize;
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

    // `put` does not offer exception safety. If it throws an exception,
    // cache may be in an inconsistent state. It is, therefore,
    // client's responsibility to handle failures correctly.
    void
    put(K const& k, V const& v)
    {
        ++mGeneration;
        CacheValue newValue{mGeneration, v};
        auto pair = mValueMap.insert(std::make_pair(k, newValue));
        if (pair.second)
        {
            // An insertion occurred: save a pointer to the pair inserted.
            // Note that it's okay to do so, as the C++14 standard
            // guarantees that rehashing does not invalidate pointers or
            // references to elements (26.2.7)
            MapValueType& inserted = *pair.first;
            mValuePtrs.push_back(&inserted);
            ++mCounters.mInserts;
            // We may have just grown over the size limit. Fix that.
            if (mValuePtrs.size() > mMaxSize)
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

    // `exists` offers strong exception safety guarantee.
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

    // `clear` does not throw
    void
    clear()
    {
        mValuePtrs.clear();
        mValueMap.clear();
    }

    // `erase_if` offers basic exception safety guarantee. If it throws an
    // exception, then the cache may or may not be modified.
    void
    erase_if(std::function<bool(V const&)> const& f)
    {
        for (size_t i = 0; i < mValuePtrs.size(); ++i)
        {
            MapValueType*& vp = mValuePtrs.at(i);
            while (mValuePtrs.size() != i && f(vp->second.mValue))
            {
                // - `erase(k)` does not throw an exception unless that
                // exception is thrown by the container’s Hash or Pred object
                // (if any)
                // - `std::swap` does not throw
                // - `std::unordered_map::pop_back` does not throw
                mValueMap.erase(vp->first);
                std::swap(vp, mValuePtrs.back());
                mValuePtrs.pop_back();
            }
        }
    }

    // `maybeGet` offers basic exception safety guarantee.
    // Returns a pointer to the value if the key exists,
    // and returns a nullptr otherwise.
    V*
    maybeGet(K const& k)
    {
        auto it = mValueMap.find(k);
        if (it != mValueMap.end())
        {
            auto& cacheVal = it->second;
            ++mCounters.mHits;
            cacheVal.mLastAccess = ++mGeneration;
            return &cacheVal.mValue;
        }
        else
        {
            ++mCounters.mMisses;
            return nullptr;
        }
    }

    // `get` offers basic exception safety guarantee.
    V&
    get(K const& k)
    {
        V* result = maybeGet(k);
        if (result == nullptr)
        {
            throw std::range_error("There is no such key in cache");
        }
        return *result;
    }
};
}
