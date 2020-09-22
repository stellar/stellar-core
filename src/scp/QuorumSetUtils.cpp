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
uint32 const MAXIMUM_QUORUM_NESTING_LEVEL = 4;

namespace
{

class QuorumSetSanityChecker
{
  public:
    explicit QuorumSetSanityChecker(SCPQuorumSet const& qSet, bool extraChecks,
                                    char const*& errString);
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

    bool checkSanity(SCPQuorumSet const& qSet, uint32 depth,
                     char const*& errString);
};

QuorumSetSanityChecker::QuorumSetSanityChecker(SCPQuorumSet const& qSet,
                                               bool extraChecks,
                                               char const*& errString)
    : mExtraChecks{extraChecks}
{
    mIsSane = checkSanity(qSet, 0, errString);
    if (mIsSane && (mCount < 1 || mCount > 1000))
    {
        mIsSane = false;
        errString =
            "Total number of nodes in a quorum must be within 1 and 1000";
    }
}

bool
QuorumSetSanityChecker::checkSanity(SCPQuorumSet const& qSet, uint32 depth,
                                    char const*& errString)
{
    if (depth > MAXIMUM_QUORUM_NESTING_LEVEL)
    {
        errString = "Maximum quorum nesting level exceeded";
        return false;
    }

    if (qSet.threshold < 1)
    {
        errString = "Threshold must be greater than 0";
        return false;
    }

    auto& v = qSet.validators;
    auto& i = qSet.innerSets;

    size_t totEntries = v.size() + i.size();
    size_t vBlockingSize = totEntries - qSet.threshold + 1;
    mCount += v.size();

    if (qSet.threshold > totEntries)
    {
        errString = "Threshold exceeds total number of entries";
        return false;
    }

    // threshold is within the proper range
    if (mExtraChecks && qSet.threshold < vBlockingSize)
    {
        errString = "Threshold is lower than the v-blocking size (< 51%).";
        return false;
    }

    for (auto const& n : v)
    {
        auto r = mKnownNodes.insert(n);
        if (!r.second)
        {
            // n was already present
            errString = "Duplicate node found in quorum configuration";
            return false;
        }
    }

    for (auto const& iSet : i)
    {
        if (!checkSanity(iSet, depth + 1, errString))
        {
            return false;
        }
    }

    return true;
}
}

bool
isQuorumSetSane(SCPQuorumSet const& qSet, bool extraChecks,
                char const*& errString)
{
    QuorumSetSanityChecker checker{qSet, extraChecks, errString};
    return checker.isSane();
}

namespace
{
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
normalizeQSetSimplify(SCPQuorumSet& qSet, NodeID const* idToRemove)
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
        normalizeQSetSimplify(*it, idToRemove);
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

template <typename InputIt1, typename InputIt2, class Compare>
int
intLexicographicalCompare(InputIt1 first1, InputIt1 last1, InputIt2 first2,
                          InputIt2 last2, Compare comp)
{
    for (; first1 != last1 && first2 != last2; first1++, first2++)
    {
        auto c = comp(*first1, *first2);
        if (c != 0)
        {
            return c;
        }
    }
    if (first1 == last1 && first2 != last2)
    {
        return -1;
    }
    if (first1 != last1 && first2 == last2)
    {
        return 1;
    }
    return 0;
}

// returns -1 if l < r ; 0 if l == r ; 1 if l > r
// lexicographical sort
// looking at, in order: validators, innerSets, threshold
int
qSetCompareInt(SCPQuorumSet const& l, SCPQuorumSet const& r)
{
    auto& lvals = l.validators;
    auto& rvals = r.validators;

    // compare by validators first
    auto res = intLexicographicalCompare(
        lvals.begin(), lvals.end(), rvals.begin(), rvals.end(),
        [](PublicKey const& l, PublicKey const& r) {
            if (l < r)
            {
                return -1;
            }
            if (r < l)
            {
                return 1;
            }
            return 0;
        });
    if (res != 0)
    {
        return res;
    }

    // then compare by inner sets
    auto const& li = l.innerSets;
    auto const& ri = r.innerSets;
    res = intLexicographicalCompare(li.begin(), li.end(), ri.begin(), ri.end(),
                                    qSetCompareInt);
    if (res != 0)
    {
        return res;
    }

    // compare by threshold
    return (l.threshold < r.threshold) ? -1
                                       : ((l.threshold == r.threshold) ? 0 : 1);
}

// helper function that reorders validators and inner sets
// in a standard way
void
normalizeQuorumSetReorder(SCPQuorumSet& qset)
{
    std::sort(qset.validators.begin(), qset.validators.end());
    for (auto& qs : qset.innerSets)
    {
        normalizeQuorumSetReorder(qs);
    }
    // now, we can reorder the inner sets
    std::sort(qset.innerSets.begin(), qset.innerSets.end(),
              [](SCPQuorumSet const& l, SCPQuorumSet const& r) {
                  return qSetCompareInt(l, r) < 0;
              });
}
}
void
normalizeQSet(SCPQuorumSet& qSet, NodeID const* idToRemove)
{
    normalizeQSetSimplify(qSet, idToRemove);
    normalizeQuorumSetReorder(qSet);
}
}
