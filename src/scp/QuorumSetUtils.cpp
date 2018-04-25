// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "QuorumSetUtils.h"

#include "util/XDROperators.h"
#include "xdr/Stellar-SCP.h"
#include "xdr/Stellar-types.h"

#include <algorithm>
#include <set>

namespace stellar
{

namespace
{

class QuorumSetSanityChecker
{
  public:
    explicit QuorumSetSanityChecker(SCPQuorumSet const& qSet, bool extraChecks);
    bool
    isSane() const
    {
        return mIsSane;
    }

  private:
    bool mExtraChecks;
    std::set<NodeID> mKnownNodes;
    bool mIsSane;
    size_t mCount{0};

    bool checkSanity(SCPQuorumSet const& qSet, int depth);
};

QuorumSetSanityChecker::QuorumSetSanityChecker(SCPQuorumSet const& qSet,
                                               bool extraChecks)
    : mExtraChecks{extraChecks}
{
    mIsSane = checkSanity(qSet, 0) && mCount >= 1 && mCount <= 1000;
}

bool
QuorumSetSanityChecker::checkSanity(SCPQuorumSet const& qSet, int depth)
{
    if (depth > 2)
        return false;

    if (qSet.threshold < 1)
        return false;

    auto& v = qSet.validators;
    auto& i = qSet.innerSets;

    size_t totEntries = v.size() + i.size();
    size_t vBlockingSize = totEntries - qSet.threshold + 1;
    mCount += v.size();

    if (qSet.threshold > totEntries)
        return false;

    // threshold is within the proper range
    if (mExtraChecks && qSet.threshold < vBlockingSize)
        return false;

    for (auto const& n : v)
    {
        auto r = mKnownNodes.insert(n);
        if (!r.second)
        {
            // n was already present
            return false;
        }
    }

    for (auto const& iSet : i)
    {
        if (!checkSanity(iSet, depth + 1))
        {
            return false;
        }
    }

    return true;
}
}

bool
isQuorumSetSane(SCPQuorumSet const& qSet, bool extraChecks)
{
    QuorumSetSanityChecker checker{qSet, extraChecks};
    return checker.isSane();
}

// helper function that:
//  * removes nodeID
//      { t: n, v: { ...BEFORE... , nodeID, ...AFTER... }, ...}
//      { t: n-1, v: { ...BEFORE..., ...AFTER...} , ... }
//  * simplifies singleton inner set into outerset
//      { t: n, v: { ... }, { t: 1, X }, ... }
//        into
//      { t: n, v: { ..., X }, .... }
//  * simplifies singleton innersets
//      { t:1, { innerSet } } into innerSet

void
normalizeQSet(SCPQuorumSet& qSet, NodeID const* idToRemove)
{
    using xdr::operator==;
    auto& v = qSet.validators;
    if (idToRemove)
    {
        auto it_v = std::remove_if(v.begin(), v.end(), [&](NodeID const& n) {
            return n == *idToRemove;
        });
        qSet.threshold -= uint32(v.end() - it_v);
        v.erase(it_v, v.end());
    }

    auto& i = qSet.innerSets;
    auto it = i.begin();
    while (it != i.end())
    {
        normalizeQSet(*it, idToRemove);
        // merge singleton inner sets into validator list
        if (it->threshold == 1 && it->validators.size() == 1 &&
            it->innerSets.size() == 0)
        {
            v.emplace_back(it->validators.front());
            it = i.erase(it);
        }
        else
        {
            it++;
        }
    }

    // simplify quorum set if needed
    if (qSet.threshold == 1 && v.size() == 0 && i.size() == 1)
    {
        auto t = qSet.innerSets.back();
        qSet = t;
    }
}
}
