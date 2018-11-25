// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/BitsetEnumerator.h"
#include <cassert>

namespace stellar
{

///////////////////////////////////////////////////////////////////////////
// ConstantEnumerator
///////////////////////////////////////////////////////////////////////////

ConstantEnumerator::ConstantEnumerator(std::bitset<64> bits)
    : mBits(bits), mDone(false)
{
}

std::shared_ptr<BitsetEnumerator>
ConstantEnumerator::bitNumber(size_t n)
{
    assert(n < 64);
    std::bitset<64> bits;
    bits.set(n);
    return std::make_shared<ConstantEnumerator>(bits);
}

std::vector<std::shared_ptr<BitsetEnumerator>>
ConstantEnumerator::bitNumbers(std::vector<size_t> ns)
{
    std::vector<std::shared_ptr<BitsetEnumerator>> ret;
    for (auto n : ns)
    {
        ret.push_back(bitNumber(n));
    }
    return ret;
}

void
ConstantEnumerator::reset()
{
    mDone = false;
}

ConstantEnumerator::operator bool() const
{
    return !mDone;
}

std::bitset<64> ConstantEnumerator::operator*() const
{
    return mBits;
}

void
ConstantEnumerator::operator++()
{
    mDone = true;
}

///////////////////////////////////////////////////////////////////////////
// PermutationEnumerator
///////////////////////////////////////////////////////////////////////////

PermutationEnumerator::PermutationEnumerator(size_t nSet, size_t nTotal)
    : mCur(0), mSet(nSet), mTot(nTotal)
{
    assert(mSet <= mTot);
    assert(mSet > 0 && mSet <= 64);
    assert(mTot > 0 && mTot <= 64);
    while (nSet-- > 0)
    {
        mCur <<= 1;
        mCur |= 1;
    }
}

void
PermutationEnumerator::reset()
{
    mCur = 0;
    auto nSet = mSet;
    while (nSet-- > 0)
    {
        mCur <<= 1;
        mCur |= 1;
    }
}

PermutationEnumerator::operator bool() const
{
    uint64_t one = 1;
    return !(mCur & ~((one << mTot) - 1));
}

std::bitset<64> PermutationEnumerator::operator*() const
{
    std::bitset<64> bits(mCur);
    assert(bits.count() == mSet);
    return bits;
}

// Simplest way of expressing unsigned unary-neg without tripping compiler
// errors, and/or hunting for the One Right Signedness Cast agreeable to
// all compilers / avoiding undefined behavior.
static inline uint64_t
uneg(uint64_t const& n)
{
    return (~n) + 1;
}

void
PermutationEnumerator::operator++()
{
    // Next bit-permutation. See:
    // https://graphics.stanford.edu/~seander/bithacks.html#NextBitPermutation
    uint64_t t = (mCur | (mCur - 1)) + 1;
    mCur = t | ((((t & uneg(t)) / (mCur & uneg(mCur))) >> 1) - 1);
}

///////////////////////////////////////////////////////////////////////////
// PowersetEnumerator
///////////////////////////////////////////////////////////////////////////

PowersetEnumerator::PowersetEnumerator(size_t nBits)
    : mCur(1), mLim(1ull << nBits)
{
    assert(nBits < 64);
}

void
PowersetEnumerator::reset()
{
    mCur = 1;
}

PowersetEnumerator::operator bool() const
{
    return mCur < mLim;
}

std::bitset<64> PowersetEnumerator::operator*() const
{
    return std::bitset<64>(mCur);
}

void
PowersetEnumerator::operator++()
{
    ++mCur;
}

///////////////////////////////////////////////////////////////////////////
// CartesianProductEnumerator
///////////////////////////////////////////////////////////////////////////

CartesianProductEnumerator::CartesianProductEnumerator(
    std::vector<std::shared_ptr<BitsetEnumerator>> innerEnums)
    : mInnerEnums(innerEnums)
{
    for (auto const& e : mInnerEnums)
    {
        e->reset();
    }
}

void
CartesianProductEnumerator::reset()
{
    for (auto& e : mInnerEnums)
    {
        e->reset();
    }
}

CartesianProductEnumerator::operator bool() const
{
    for (auto const& e : mInnerEnums)
    {
        if (*e)
        {
            return true;
        }
    }
    return false;
}

std::bitset<64> CartesianProductEnumerator::operator*() const
{
    std::bitset<64> tmp;
    for (auto const& e : mInnerEnums)
    {
        tmp |= **e;
    }
    return tmp;
}

void
CartesianProductEnumerator::operator++()
{
    // Want to walk along the array looking for the first
    // element that wasn't done, but becomes done when we
    // increment it.
    for (size_t i = 0; i < mInnerEnums.size(); ++i)
    {
        auto curr = mInnerEnums[i];
        if (!(*curr))
        {
            continue;
        }
        // enumerator i is 'true', so now advance it and see if it
        // went false.
        ++(*curr);
        if (*curr)
        {
            // It's still got life in it, stop let it go.
            return;
        }
        else
        {
            // We just exhausted enumerator i, meaning we need to
            // "carry" the advance to the next one, and if it
            // remains live after the carry, reset enumerator i and
            // all the previous ones.
            for (size_t carry = i + 1; carry < mInnerEnums.size(); ++carry)
            {
                auto next = mInnerEnums[carry];
                ++(*next);
                if (*next)
                {
                    for (size_t reset = 0; reset <= i; ++reset)
                    {
                        mInnerEnums[i]->reset();
                    }
                    return;
                }
            }
        }
    }
}

///////////////////////////////////////////////////////////////////////////
// SelectionEnumerator
///////////////////////////////////////////////////////////////////////////

SelectionEnumerator::SelectionEnumerator(
    std::shared_ptr<BitsetEnumerator> index,
    std::vector<std::shared_ptr<BitsetEnumerator>> const& innerEnums)
    : mInnerEnums(innerEnums)
    , mIndexEnum(index)
    , mProduct(select(index, mInnerEnums))
{
    for (auto const& e : mInnerEnums)
    {
        e->reset();
    }
}

std::shared_ptr<BitsetEnumerator>
SelectionEnumerator::bitNumbers(size_t nSel, std::vector<size_t> ns)
{
    auto idx = std::make_shared<PermutationEnumerator>(nSel, ns.size());
    auto ces = ConstantEnumerator::bitNumbers(ns);
    return std::make_shared<SelectionEnumerator>(idx, ces);
}

CartesianProductEnumerator
SelectionEnumerator::select(
    std::shared_ptr<BitsetEnumerator> index,
    std::vector<std::shared_ptr<BitsetEnumerator>> const& from)
{
    std::bitset<64> bits(**index);
    std::vector<std::shared_ptr<BitsetEnumerator>> active;
    for (size_t i = 0; i < 64; ++i)
    {
        if (i >= from.size())
        {
            break;
        }
        if (bits[i])
        {
            active.push_back(from[i]);
        }
    }
    return CartesianProductEnumerator(active);
}

std::bitset<64> SelectionEnumerator::operator*() const
{
    return *mProduct;
}

void
SelectionEnumerator::reset()
{
    mIndexEnum->reset();
    mProduct = select(mIndexEnum, mInnerEnums);
}

SelectionEnumerator::operator bool() const
{
    return mProduct || *mIndexEnum;
}

void
SelectionEnumerator::operator++()
{
    if (mProduct)
    {
        ++mProduct;
    }
    if (!mProduct)
    {
        ++(*mIndexEnum);
        if (*mIndexEnum)
        {
            mProduct = select(mIndexEnum, mInnerEnums);
        }
    }
}
}
