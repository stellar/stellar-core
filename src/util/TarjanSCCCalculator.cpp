// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/TarjanSCCCalculator.h"
#include "util/Logging.h"

// This is a completely stock implementation of Tarjan's algorithm for
// calculating strongly connected components. Like "read off of wikipedia"
// stock. Go have a look!
//
// https://en.wikipedia.org/wiki/Tarjan%27s_strongly_connected_components_algorithm

TarjanSCCCalculator::TarjanSCCCalculator()
{
}

void
TarjanSCCCalculator::calculateSCCs(
    size_t graphSize,
    std::function<BitSet const&(size_t)> const& getNodeSuccessors)
{
    mNodes.clear();
    mStack.clear();
    mIndex = 0;
    mSCCs.clear();
    for (size_t i = 0; i < graphSize; ++i)
    {
        mNodes.emplace_back(SCCNode{});
    }
    for (size_t i = 0; i < graphSize; ++i)
    {
        if (mNodes.at(i).mIndex == -1)
        {
            scc(i, getNodeSuccessors);
        }
    }
}

void
TarjanSCCCalculator::scc(
    size_t i, std::function<BitSet const&(size_t)> const& getNodeSuccessors)
{
    auto& v = mNodes.at(i);
    v.mIndex = mIndex;
    v.mLowLink = mIndex;
    mIndex++;
    mStack.push_back(i);
    v.mOnStack = true;

    BitSet const& succ = getNodeSuccessors(i);
    for (size_t j = 0; succ.nextSet(j); ++j)
    {
        LOG_TRACE(DEFAULT_LOG, "TarjanSCC edge: {} -> {}", i, j);
        SCCNode& w = mNodes.at(j);
        if (w.mIndex == -1)
        {
            scc(j, getNodeSuccessors);
            v.mLowLink = std::min(v.mLowLink, w.mLowLink);
        }
        else if (w.mOnStack)
        {
            v.mLowLink = std::min(v.mLowLink, w.mIndex);
        }
    }

    if (v.mLowLink == v.mIndex)
    {
        BitSet newScc;
        newScc.set(i);
        size_t j = 0;
        do
        {
            j = mStack.back();
            newScc.set(j);
            mStack.pop_back();
            mNodes.at(j).mOnStack = false;
        } while (j != i);
        LOG_TRACE(DEFAULT_LOG, "TarjanSCC SCC: {}", newScc);
        mSCCs.push_back(newScc);
    }
}
