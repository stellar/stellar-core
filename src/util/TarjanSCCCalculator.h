#pragma once

// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <cstdint>
#include <functional>
#include <vector>

#include "util/BitSet.h"

// Implementation of Tarjan's algorithm for SCC calculation.
struct TarjanSCCCalculator
{
    struct SCCNode
    {
        int mIndex = {-1};
        int mLowLink = {-1};
        bool mOnStack = {false};
    };

    std::vector<SCCNode> mNodes;
    std::vector<size_t> mStack;
    int mIndex = {0};
    std::vector<BitSet> mSCCs;

    TarjanSCCCalculator();
    void calculateSCCs(
        size_t graphSize,
        std::function<BitSet const&(size_t)> const& getNodeSuccessors);
    void scc(size_t i,
             std::function<BitSet const&(size_t)> const& getNodeSuccessors);
};