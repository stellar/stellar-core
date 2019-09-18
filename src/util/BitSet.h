#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// C++ value-semantic / convenience wrapper around C bitset_t

#include <functional>
#include <memory>
#include <ostream>
#include <set>

extern "C" {
#include "util/cbitset.h"
};

class BitSet
{
    mutable bool mCountDirty = {true};
    mutable size_t mCount = {0};
    std::unique_ptr<bitset_t, decltype(&bitset_free)> mPtr;

  public:
    BitSet() : mPtr(bitset_create(), &bitset_free)
    {
    }
    BitSet(size_t n) : mPtr(bitset_create_with_capacity(n), &bitset_free)
    {
    }
    BitSet(std::set<size_t> const& s)
        : mPtr(bitset_create_with_capacity(s.empty() ? 0 : *s.rbegin()),
               &bitset_free)
    {
        for (auto i : s)
            set(i);
    }
    BitSet(BitSet const& other)
        : mPtr(bitset_copy(other.mPtr.get()), &bitset_free)
    {
    }
    BitSet&
    operator=(BitSet const& other)
    {
        mPtr = decltype(mPtr)(bitset_copy(other.mPtr.get()), &bitset_free);
        mCount = other.mCount;
        mCountDirty = other.mCountDirty;
        return *this;
    }
    BitSet(BitSet&& other) = default;
    BitSet& operator=(BitSet&& other) = default;

    bool
    operator!=(BitSet const& other) const
    {
        return !((*this) == other);
    }

    bool
    operator==(BitSet const& other) const
    {
        return bitset_equal(mPtr.get(), other.mPtr.get());
    }

    bool
    isSubsetEq(BitSet const& other) const
    {
        return bitset_subseteq(mPtr.get(), other.mPtr.get());
    }

    bool
    operator<=(BitSet const& other) const
    {
        return isSubsetEq(other);
    }

    size_t
    size() const
    {
        return bitset_size_in_bits(mPtr.get());
    }
    void
    set(size_t i)
    {
        bitset_set(mPtr.get(), i);
        mCountDirty = true;
    }
    void
    unset(size_t i)
    {
        bitset_unset(mPtr.get(), i);
        mCountDirty = true;
    }
    bool
    get(size_t i) const
    {
        return bitset_get(mPtr.get(), i);
    }
    void
    clear()
    {
        bitset_clear(mPtr.get());
        mCount = 0;
        mCountDirty = false;
    }

    size_t
    count() const
    {
        if (mCountDirty)
        {
            mCount = bitset_count(mPtr.get());
            mCountDirty = false;
        }
        return mCount;
    }
    bool
    empty() const
    {
        size_t tmp = 0;
        return !nextSet(tmp);
    }
    operator bool() const
    {
        return !empty();
    }
    size_t
    min() const
    {
        return bitset_minimum(mPtr.get());
    }
    size_t
    max() const
    {
        return bitset_maximum(mPtr.get());
    }

    void
    inplaceUnion(BitSet const& other)
    {
        bitset_inplace_union(mPtr.get(), other.mPtr.get());
        mCountDirty = true;
    }
    BitSet
    operator|(BitSet const& other) const
    {
        BitSet tmp(*this);
        tmp.inplaceUnion(other);
        return tmp;
    }
    void
    operator|=(BitSet const& other)
    {
        inplaceUnion(other);
    }

    void
    inplaceIntersection(BitSet const& other)
    {
        bitset_inplace_intersection(mPtr.get(), other.mPtr.get());
        mCountDirty = true;
    }
    BitSet operator&(BitSet const& other) const
    {
        BitSet tmp(*this);
        tmp.inplaceIntersection(other);
        return tmp;
    }
    void
    operator&=(BitSet const& other)
    {
        inplaceIntersection(other);
    }

    void
    inplaceDifference(BitSet const& other)
    {
        bitset_inplace_difference(mPtr.get(), other.mPtr.get());
        mCountDirty = true;
    }
    BitSet
    operator-(BitSet const& other) const
    {
        BitSet tmp(*this);
        tmp.inplaceDifference(other);
        return tmp;
    }
    void
    operator-=(BitSet const& other)
    {
        inplaceDifference(other);
    }

    void
    inplaceSymmetricDifference(BitSet const& other)
    {
        bitset_inplace_symmetric_difference(mPtr.get(), other.mPtr.get());
        mCountDirty = true;
    }
    BitSet
    symmetricDifference(BitSet const& other) const
    {
        BitSet tmp(*this);
        tmp.inplaceSymmetricDifference(other);
        return tmp;
    }

    size_t
    unionCount(BitSet const& other) const
    {
        return bitset_union_count(mPtr.get(), other.mPtr.get());
    }
    size_t
    intersectionCount(BitSet const& other) const
    {
        return bitset_intersection_count(mPtr.get(), other.mPtr.get());
    }
    size_t
    differenceCount(BitSet const& other) const
    {
        return bitset_difference_count(mPtr.get(), other.mPtr.get());
    }
    size_t
    symmetricDifferenceCount(BitSet const& other) const
    {
        return bitset_symmetric_difference_count(mPtr.get(), other.mPtr.get());
    }
    bool
    nextSet(size_t& i) const
    {
        return nextSetBit(mPtr.get(), &i);
    }
    void
    streamWith(std::ostream& out,
               std::function<void(std::ostream&, size_t)> item) const
    {
        out << '{';
        bool first = true;
        for (size_t i = 0; nextSet(i); ++i)
        {
            if (first)
            {
                first = false;
            }
            else
            {
                out << ", ";
            }
            item(out, i);
        }
        out << '}';
    }
    void
    stream(std::ostream& out) const
    {
        streamWith(out, [](std::ostream& out, size_t i) { out << i; });
    }
};

inline std::ostream&
operator<<(std::ostream& out, BitSet const& b)
{
    b.stream(out);
    return out;
}
