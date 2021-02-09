#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// C++ value-semantic / convenience wrapper around C bitset_t

#include "util/Logging.h"
#include <functional>
#include <memory>
#include <ostream>
#include <set>

extern "C"
{
#include "util/cbitset.h"
};

class BitSet
{
    // Value-semantic wrapper for cbitset that carries a small inline bitset
    // around with it for even less heap allocation / more cache-friendliness.
    // Adjust the INLINE_NWORDS as necessary; it'll still work (just slow down
    // a bit) if you guess wrong.
    static constexpr size_t WORD_BITS_LOG2 = 6; // 2^6 = 64
    static constexpr size_t WORD_BITS = (1 << WORD_BITS_LOG2);
    static_assert(WORD_BITS == (8 * sizeof(uint64_t)), "unexpected WORD_BITS");
    static constexpr size_t INLINE_NWORDS = 1;
    static constexpr size_t INLINE_NBITS = INLINE_NWORDS * WORD_BITS;
    mutable bool mCountDirty = {true};
    mutable size_t mCount = {0};

    // If mPtr == &mInlineBitset then we are using the inline bitset
    // (mInlineBitset.array === &mInlineBits) and do not need to free either the
    // desriptor or the array. If mPtr != &mInlineBitset then it's pointing to
    // an out-of-line bitset which we need to free.
    bitset_t* mPtr{nullptr};
    bitset_t mInlineBitset{nullptr, INLINE_NWORDS, INLINE_NWORDS};
    uint64_t mInlineBits[INLINE_NWORDS]{0};

    bool
    isStoredInline() const
    {
        return mPtr == &mInlineBitset;
    }

    void
    setToEmptyAndInline()
    {
        if (mPtr && !isStoredInline())
        {
            bitset_free(mPtr);
        }
        mPtr = &mInlineBitset;
        for (size_t i = 0; i < INLINE_NWORDS; ++i)
        {
            mInlineBits[i] = 0;
        }
        mInlineBitset.array = mInlineBits;
    }

    void
    ensureCapacity(size_t nBits)
    {
        size_t nWords = (nBits + WORD_BITS - 1) >> WORD_BITS_LOG2;
        if (nWords <= mPtr->capacity)
        {
            return;
        }
        if (isStoredInline())
        {
            // Promote inline to out-of-line, no free required.
            mPtr = bitset_copy(mPtr);
        }
        bitset_resize(mPtr, nWords, true);
    }

    void
    setToEmptyWithCapacity(size_t nBits)
    {
        // Equal to "setToEmptyAndInline + ensureCapacity" but with one malloc
        // rather than malloc+realloc in the case where it's not inline.
        setToEmptyAndInline();
        if (nBits > INLINE_NBITS)
        {
            mPtr = bitset_create_with_capacity(nBits);
        }
    }

    void
    copyOther(BitSet const& other)
    {
        // This step will also free any out-of-line bitset_t we own.
        setToEmptyAndInline();

        if (other.isStoredInline())
        {
            for (size_t i = 0; i < INLINE_NWORDS; ++i)
            {
                mInlineBits[i] = other.mInlineBits[i];
            }
        }
        else
        {
            mPtr = bitset_copy(other.mPtr);
        }
        mCount = other.mCount;
        mCountDirty = other.mCountDirty;
    }

  public:
    ~BitSet()
    {
        setToEmptyAndInline();
    }
    BitSet()
    {
        setToEmptyAndInline();
    }
    BitSet(size_t n)
    {
        setToEmptyWithCapacity(n);
    }
    BitSet(std::set<size_t> const& s)
    {
        setToEmptyWithCapacity(s.empty() ? 0 : *s.rbegin());
        for (auto i : s)
        {
            set(i);
        }
    }
    BitSet(BitSet const& other)
    {
        copyOther(other);
    }
    BitSet&
    operator=(BitSet const& other)
    {
        copyOther(other);
        return *this;
    }

    bool
    operator!=(BitSet const& other) const
    {
        return !((*this) == other);
    }

    bool
    operator==(BitSet const& other) const
    {
        return bitset_equal(mPtr, other.mPtr);
    }

    bool
    isSubsetEq(BitSet const& other) const
    {
        return bitset_subseteq(mPtr, other.mPtr);
    }

    size_t
    size() const
    {
        return bitset_size_in_bits(mPtr);
    }
    void
    set(size_t i)
    {
        ensureCapacity(i);
        bitset_set(mPtr, i);
        mCountDirty = true;
    }
    void
    unset(size_t i)
    {
        bitset_unset(mPtr, i);
        mCountDirty = true;
    }
    bool
    get(size_t i) const
    {
        return bitset_get(mPtr, i);
    }
    void
    clear()
    {
        bitset_clear(mPtr);
        mCount = 0;
        mCountDirty = false;
    }

    size_t
    count() const
    {
        if (mCountDirty)
        {
            mCount = bitset_count(mPtr);
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
    size_t
    min() const
    {
        return bitset_minimum(mPtr);
    }
    size_t
    max() const
    {
        return bitset_maximum(mPtr);
    }

    void
    inplaceUnion(BitSet const& other)
    {
        ensureCapacity(other.size());
        bitset_inplace_union(mPtr, other.mPtr);
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
        // We do not need to do ensureCapacity() here because
        // intersection never grows a bitset: no reallocation.
        bitset_inplace_intersection(mPtr, other.mPtr);
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
        // We do not need to do ensureCapacity() here because
        // difference never grows a bitset: no reallocation.
        bitset_inplace_difference(mPtr, other.mPtr);
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
        ensureCapacity(other.size());
        bitset_inplace_symmetric_difference(mPtr, other.mPtr);
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
        return bitset_union_count(mPtr, other.mPtr);
    }
    size_t
    intersectionCount(BitSet const& other) const
    {
        return bitset_intersection_count(mPtr, other.mPtr);
    }
    size_t
    differenceCount(BitSet const& other) const
    {
        return bitset_difference_count(mPtr, other.mPtr);
    }
    size_t
    symmetricDifferenceCount(BitSet const& other) const
    {
        return bitset_symmetric_difference_count(mPtr, other.mPtr);
    }
    bool
    nextSet(size_t& i) const
    {
        return nextSetBit(mPtr, &i);
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
    class HashFunction
    {
        std::hash<uint64_t> mHasher;

      public:
        size_t
        operator()(BitSet const& bitset) const noexcept
        {
            // Implementation taken from Boost.
            // https://www.boost.org/doc/libs/1_35_0/doc/html/boost/hash_combine_id241013.html
            size_t seed = 0;
            for (size_t i = 0; i < bitset.mPtr->arraysize; i++)
            {
                seed ^= mHasher(bitset.mPtr->array[i]) + 0x9e3779b9 +
                        (seed << 6) + (seed >> 2);
            }
            return seed;
        }
    };
};

inline std::ostream&
operator<<(std::ostream& out, BitSet const& b)
{
    b.stream(out);
    return out;
}
