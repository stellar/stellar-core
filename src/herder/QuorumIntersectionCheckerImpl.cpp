// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "QuorumIntersectionCheckerImpl.h"
#include "QuorumIntersectionChecker.h"

#include "util/Logging.h"
#include "util/Math.h"

namespace
{

////////////////////////////////////////////////////////////////////////////////
// Implementation of QBitSet
////////////////////////////////////////////////////////////////////////////////

using namespace stellar;
struct QBitSet;
using QGraph = std::vector<QBitSet>;

QBitSet::QBitSet(uint32_t threshold, BitSet const& nodes,
                 QGraph const& innerSets)
    : mThreshold(threshold)
    , mNodes(nodes)
    , mInnerSets(innerSets)
    , mAllSuccessors(getSuccessors(nodes, innerSets))
{
}

void
QBitSet::log(size_t indent) const
{
    std::string s(indent, ' ');
    CLOG_DEBUG(SCP, "{}QBitSet: thresh={}/{} validators={}", s, mThreshold,
               (mNodes.count() + mInnerSets.size()), mNodes);
    for (auto const& inner : mInnerSets)
    {
        inner.log(indent + 4);
    }
}

BitSet
QBitSet::getSuccessors(BitSet const& nodes, QGraph const& inner)
{
    BitSet out(nodes);
    for (auto const& i : inner)
    {
        out |= i.mAllSuccessors;
    }
    return out;
}

////////////////////////////////////////////////////////////////////////////////
// Implementation of TarjanSCCCalculator
////////////////////////////////////////////////////////////////////////////////
//
// This is a completely stock implementation of Tarjan's algorithm for
// calculating strongly connected components. Like "read off of wikipedia"
// stock. Go have a look!
//
// https://en.wikipedia.org/wiki/Tarjan%27s_strongly_connected_components_algorithm

TarjanSCCCalculator::TarjanSCCCalculator(QGraph const& graph) : mGraph(graph)
{
}

void
TarjanSCCCalculator::calculateSCCs()
{
    mNodes.clear();
    mStack.clear();
    mIndex = 0;
    mSCCs.clear();
    for (size_t i = 0; i < mGraph.size(); ++i)
    {
        mNodes.emplace_back(SCCNode{});
    }
    for (size_t i = 0; i < mGraph.size(); ++i)
    {
        if (mNodes.at(i).mIndex == -1)
        {
            scc(i);
        }
    }
}

void
TarjanSCCCalculator::scc(size_t i)
{
    auto& v = mNodes.at(i);
    v.mIndex = mIndex;
    v.mLowLink = mIndex;
    mIndex++;
    mStack.push_back(i);
    v.mOnStack = true;

    BitSet const& succ = mGraph.at(i).mAllSuccessors;
    for (size_t j = 0; succ.nextSet(j); ++j)
    {
        CLOG_TRACE(SCP, "edge: {} -> {}", i, j);
        SCCNode& w = mNodes.at(j);
        if (w.mIndex == -1)
        {
            scc(j);
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
        mSCCs.push_back(newScc);
    }
}

////////////////////////////////////////////////////////////////////////////////
// Implementation of MinQuorumEnumerator
////////////////////////////////////////////////////////////////////////////////

// Slightly tweaked variant of Lachowski's next-node function.
size_t
MinQuorumEnumerator::pickSplitNode() const
{
    std::vector<size_t>& inDegrees = mQic.mInDegrees;
    inDegrees.assign(mQic.mGraph.size(), 0);
    assert(!mRemaining.empty());
    size_t maxNode = mRemaining.max();
    size_t maxCount = 1;
    size_t maxDegree = 0;
    for (size_t i = 0; mRemaining.nextSet(i); ++i)
    {

        // Heuristic opportunity: biasing towards cross-org edges and
        // away from intra-org edges seems to help; work out some way
        // to make this a robust bias.
        BitSet avail = mQic.mGraph.at(i).mAllSuccessors & mRemaining;
        for (size_t j = 0; avail.nextSet(j); ++j)
        {
            size_t currDegree = ++inDegrees.at(j);
            if (currDegree >= maxDegree)
            {
                if (currDegree == maxDegree)
                {
                    // currDegree same as existing max: replace it
                    // only probabilistically.
                    maxCount++;
                    if (rand_uniform<size_t>(0, maxCount) == 0)
                    {
                        // Not switching max element with max degree.
                        continue;
                    }
                    // Switching max element with max degree.
                }
                else
                {
                    // currDegree strictly greater, reset replica count.
                    maxCount = 1;
                }
                maxDegree = currDegree;
                maxNode = j;
            }
        }
    }
    return maxNode;
}

size_t
MinQuorumEnumerator::maxCommit() const
{
    return mScanSCC.count() / 2;
}

MinQuorumEnumerator::MinQuorumEnumerator(
    BitSet const& committed, BitSet const& remaining, BitSet const& scanSCC,
    QuorumIntersectionCheckerImpl const& qic)
    : mCommitted(committed)
    , mRemaining(remaining)
    , mPerimeter(committed | remaining)
    , mScanSCC(scanSCC)
    , mQic(qic)
{
}

bool
MinQuorumEnumerator::anyMinQuorumHasDisjointQuorum()
{
    if (mQic.mInterruptFlag)
    {
        throw QuorumIntersectionChecker::InterruptedException();
    }

    mQic.mStats.mCallsStarted++;

    // Emit a progress meter every million calls.
    if ((mQic.mStats.mCallsStarted & 0xfffff) == 0)
    {
        mQic.mStats.log();
    }
    if (mQic.mLogTrace)
    {
        CLOG_TRACE(SCP, "exploring with committed={}", mCommitted);
        CLOG_TRACE(SCP, "exploring with remaining={}", mRemaining);
    }

    // First early exit: we can avoid looking for further min-quorums if
    // we're committed to more than half the SCC plus 1: the other branches
    // of the search will find them instead, within the complement of a
    // min-quorum they find (if they find any).
    if (mCommitted.count() > maxCommit())
    {
        mQic.mStats.mEarlyExit1s++;
        if (mQic.mLogTrace)
        {
            CLOG_TRACE(SCP, "early exit 1, with committed={}", mCommitted);
        }
        return false;
    }

    // Principal enumeration branch and third early exit: stop when
    // committed has grown to a quorum, enumerating it if it's a
    // min-quorum. Whether it's a min-quorum or just a normal quorum, any
    // extension _won't_ be a min-quorum, since it will have this quorum as
    // a subquorum, so both cases are terminal.
    if (mQic.mLogTrace)
    {
        CLOG_TRACE(SCP, "checking for quorum in committed={}", mCommitted);
    }
    auto committedQuorum = mQic.contractToMaximalQuorum(mCommitted);
    if (!committedQuorum.empty())
    {
        if (mQic.isMinimalQuorum(committedQuorum))
        {
            // Found a min-quorum. Examine it to see if
            // there's a disjoint quorum.
            if (mQic.mLogTrace)
            {
                CLOG_TRACE(SCP, "early exit 3.1: minimal quorum={}",
                           committedQuorum);
            }
            mQic.mStats.mEarlyExit31s++;
            return hasDisjointQuorum(committedQuorum);
        }
        if (mQic.mLogTrace)
        {
            CLOG_TRACE(SCP, "early exit 3.2: non-minimal quorum={}",
                       committedQuorum);
        }
        mQic.mStats.mEarlyExit32s++;
        return false;
    }

    // Second early exit: stop if there isn't at least one quorum to
    // enumerate in the remaining perimeter that's an extension of the
    // existing committed set.
    if (mQic.mLogTrace)
    {
        CLOG_TRACE(SCP, "checking for quorum in perimeter={}", mPerimeter);
    }
    auto extensionQuorum = mQic.contractToMaximalQuorum(mPerimeter);
    if (!extensionQuorum.empty())
    {
        if (!mCommitted.isSubsetEq(extensionQuorum))
        {
            if (mQic.mLogTrace)
            {
                CLOG_TRACE(
                    SCP,
                    "early exit 2.2: extension quorum={} in perimeter={} "
                    "does not extend committed={}",
                    extensionQuorum, mPerimeter, mCommitted);
            }
            mQic.mStats.mEarlyExit22s++;
            return false;
        }
    }
    else
    {
        if (mQic.mLogTrace)
        {
            CLOG_TRACE(SCP,
                       "early exit 2.1: no extension quorum in perimeter={}",
                       mPerimeter);
        }
        mQic.mStats.mEarlyExit21s++;
        return false;
    }

    // Principal termination condition: stop when remainder is empty.
    if (mRemaining.empty())
    {
        mQic.mStats.mTerminations++;
        if (mQic.mLogTrace)
        {
            CLOG_TRACE(SCP, "remainder exhausted");
        }
        return false;
    }

    // Phase two: recurse into subproblems.
    size_t split = pickSplitNode();
    if (mQic.mLogTrace)
    {
        CLOG_TRACE(SCP, "recursing into subproblems, split={}", split);
    }
    mRemaining.unset(split);
    MinQuorumEnumerator childExcludingSplit(mCommitted, mRemaining, mScanSCC,
                                            mQic);
    mQic.mStats.mFirstRecursionsTaken++;
    if (childExcludingSplit.anyMinQuorumHasDisjointQuorum())
    {
        if (mQic.mLogTrace)
        {
            CLOG_TRACE(SCP, "first subproblem returned true, missing split={}",
                       split);
        }
        return true;
    }
    mCommitted.set(split);
    MinQuorumEnumerator childIncludingSplit(mCommitted, mRemaining, mScanSCC,
                                            mQic);
    mQic.mStats.mSecondRecursionsTaken++;
    return childIncludingSplit.anyMinQuorumHasDisjointQuorum();
}

////////////////////////////////////////////////////////////////////////////////
// Implementation of QuorumIntersectionChecker
////////////////////////////////////////////////////////////////////////////////

QuorumIntersectionCheckerImpl::QuorumIntersectionCheckerImpl(
    QuorumTracker::QuorumMap const& qmap, Config const& cfg,
    std::atomic<bool>& interruptFlag, bool quiet)
    : mCfg(cfg)
    , mLogTrace(Logging::logTrace("SCP"))
    , mQuiet(quiet)
    , mTSC(mGraph)
    , mInterruptFlag(interruptFlag)
    , mCachedQuorums(MAX_CACHED_QUORUMS_SIZE)
{
    buildGraph(qmap);
    buildSCCs();
}

std::pair<std::vector<PublicKey>, std::vector<PublicKey>>
QuorumIntersectionCheckerImpl::getPotentialSplit() const
{
    return mPotentialSplit;
}

size_t
QuorumIntersectionCheckerImpl::getMaxQuorumsFound() const
{
    return mStats.mMaxQuorumsSeen;
}

void
QuorumIntersectionCheckerImpl::Stats::log() const
{
    CLOG_DEBUG(SCP, "Quorum intersection checker stats:");
    size_t exits = (mEarlyExit1s + mEarlyExit21s + mEarlyExit22s +
                    mEarlyExit31s + mEarlyExit32s);
    CLOG_DEBUG(SCP,
               "[Nodes: {}, SCCs: {}, ScanSCC: {}, MaxQs:{}, MinQs:{}, "
               "Calls:{}, Terms:{}, Exits:{}]",
               mTotalNodes, mNumSCCs, mScanSCCSize, mMaxQuorumsSeen,
               mMinQuorumsSeen, mCallsStarted, mTerminations, exits);
    CLOG_DEBUG(SCP, "Detailed exit stats:");
    CLOG_DEBUG(SCP, "[X1:{}, X2.1:{}, X2.2:{}, X3.1:{}, X3.2:{}]", mEarlyExit1s,
               mEarlyExit21s, mEarlyExit22s, mEarlyExit31s, mEarlyExit32s);
}

// This function is the innermost call in the checker and must be as fast
// as possible. We spend almost all of our time in here.
bool
QuorumIntersectionCheckerImpl::containsQuorumSlice(BitSet const& bs,
                                                   QBitSet const& qbs) const
{
    // First we do a very quick check: do we have enough bits in 'bs'
    // intersected with the top-level set of nodes to meet the threshold for
    // this qset?
    size_t intersecting = bs.intersectionCount(qbs.mNodes);
    if (intersecting >= qbs.mThreshold)
    {
        return true;
    }

    // If not, the residual "inner threshold" is the number of additional hits
    // (in the innerSets) we need to satisfy this qset. If there aren't enough
    // innerSets for this to be possible, we can fail immediately.
    size_t innerThreshold = qbs.mThreshold - intersecting;
    if (innerThreshold > qbs.mInnerSets.size())
    {
        return false;
    }

    // Then a second quick-ish check: do we have enough bits in 'bs' intersected
    // with the union of all the successor nodes (of all innerSets) in this qset
    // to reach the threshold? This is an overapproximation of the failure case:
    // a negative result here means that even if each of the innerSets was
    // satisfied by a single bit from any of their children that intersect 'bs',
    // we still couldn't reach threshold, so there's no point looking at them in
    // finer detail.
    if (bs.intersectionCount(qbs.mAllSuccessors) < qbs.mThreshold)
    {
        return false;
    }

    // To make the testing loop below a little faster still, we track both a
    // success limit -- the innerThreshold -- and a fail limit. This is the
    // number of innerSets we need to have _negative_ results on before we can
    // return a conclusive no.
    //
    // If we had a threshold of (say) 5 of 7, the fail-limit would be 3: once
    // we've failed 3 innerSets we can stop looking at the others since there's
    // no way to get to 5 successes.
    size_t innerFailLimit = qbs.mInnerSets.size() - innerThreshold + 1;
    for (auto const& inner : qbs.mInnerSets)
    {
        if (containsQuorumSlice(bs, inner))
        {
            innerThreshold--;
            if (innerThreshold == 0)
            {
                return true;
            }
        }
        else
        {
            innerFailLimit--;
            if (innerFailLimit == 0)
            {
                return false;
            }
        }
    }
    return false;
}

bool
QuorumIntersectionCheckerImpl::containsQuorumSliceForNode(BitSet const& bs,
                                                          size_t node) const
{
    if (!bs.get(node))
    {
        return false;
    }
    return containsQuorumSlice(bs, mGraph.at(node));
}

bool
QuorumIntersectionCheckerImpl::isAQuorum(BitSet const& nodes) const
{
    bool* pRes = mCachedQuorums.maybeGet(nodes);
    if (pRes == nullptr)
    {
        bool result = !contractToMaximalQuorum(nodes).empty();
        mCachedQuorums.put(nodes, result);
        return result;
    }
    else
    {
        return *pRes;
    }
}

BitSet
QuorumIntersectionCheckerImpl::contractToMaximalQuorum(BitSet nodes) const
{
    // Find greatest fixpoint of f(X) = {n ∈ X | containsQuorumSliceForNode(X,
    // n)}
    if (mLogTrace)
    {
        CLOG_TRACE(SCP, "Contracting to max quorum of {}", nodes);
    }
    while (true)
    {
        BitSet filtered(nodes);
        for (size_t i = 0; nodes.nextSet(i); ++i)
        {
            if (containsQuorumSliceForNode(filtered, i))
            {
                if (mLogTrace)
                {
                    CLOG_TRACE(SCP, "Have qslice for {}", i);
                }
            }
            else
            {
                if (mLogTrace)
                {
                    CLOG_TRACE(SCP, "Missing qslice for {}", i);
                }
                filtered.unset(i);
            }
        }
        if (filtered.count() == nodes.count() || filtered.empty())
        {
            if (mLogTrace)
            {
                CLOG_TRACE(SCP, "Contracted to max quorum {}", filtered);
            }
            if (!filtered.empty())
            {
                ++mStats.mMaxQuorumsSeen;
            }
            return filtered;
        }
        nodes = filtered;
    }
}

bool
QuorumIntersectionCheckerImpl::isMinimalQuorum(BitSet const& nodes) const
{
#ifndef NDEBUG
    // We should only be called with a quorum, such that contracting to its
    // maximum doesn't do anything. This is a slightly expensive check.
    assert(contractToMaximalQuorum(nodes) == nodes);
#endif

    BitSet minQ = nodes;
    if (nodes.empty())
    {
        // nodes isn't a quorum at all: certainly not a minq.
        return false;
    }
    for (size_t i = 0; nodes.nextSet(i); ++i)
    {
        minQ.unset(i);
        if (isAQuorum(minQ))
        {
            // There's a subquorum with i removed: nodes isn't a minq.
            return false;
        }
        // Restore bit for next iteration.
        minQ.set(i);
    }
    // Tried every possible one-node-less subset, found no subquorums: this one
    // is minimal.
    mStats.mMinQuorumsSeen++;
    return true;
}

void
QuorumIntersectionCheckerImpl::noteFoundDisjointQuorums(
    BitSet const& nodes, BitSet const& disj) const
{
    mPotentialSplit.first.clear();
    mPotentialSplit.second.clear();

    // Show internal node IDs only in DEBUG message; user is going to care
    // more about the translated names printed in the ERROR below.
    CLOG_DEBUG(SCP, "Disjoint quorum IDs: {} vs. {}", nodes, disj);

    std::ostringstream err;
    err << "Found potential disjoint quorums: ";
    nodes.streamWith(err, [this](std::ostream& out, size_t i) {
        out << this->nodeName(i);
        this->mPotentialSplit.first.emplace_back(this->mBitNumPubKeys.at(i));
    });
    err << " vs. ";
    disj.streamWith(err, [this](std::ostream& out, size_t i) {
        out << this->nodeName(i);
        this->mPotentialSplit.second.emplace_back(this->mBitNumPubKeys.at(i));
    });
    if (!mQuiet)
    {
        CLOG_ERROR(SCP, "{}", err.str());
    }
}

bool
MinQuorumEnumerator::hasDisjointQuorum(BitSet const& nodes) const
{
    BitSet disj = mQic.contractToMaximalQuorum(mScanSCC - nodes);
    if (!disj.empty())
    {
        mQic.noteFoundDisjointQuorums(nodes, disj);
    }
    else
    {
        if (mQic.mLogTrace)
        {
            CLOG_TRACE(SCP, "no quorum in complement  = {}",
                       (mScanSCC - nodes));
        }
    }
    return !disj.empty();
}

QBitSet
QuorumIntersectionCheckerImpl::convertSCPQuorumSet(SCPQuorumSet const& sqs)
{
    uint32_t threshold = sqs.threshold;
    BitSet nodeBits(mPubKeyBitNums.size());
    for (auto const& v : sqs.validators)
    {
        auto i = mPubKeyBitNums.find(v);
        if (i == mPubKeyBitNums.end())
        {
            // This node 'v' is one we do not have a qset for. We treat this as
            // meaning 'v' is dead: people depend on it but it's not voting, so
            // it's never a member of anyone's quorum. This is accomplished by
            // not assigning it a bit number (in buildGraph below) and not
            // adding anything to the dependents' qsets here. They will have to
            // reach their threshold using other nodes in their qset.
            //
            // This is one of 3 plausible over-approximations we could make
            // for nodes we don't have qsets for:
            //
            //   1. Treat them as dead (what we're doing here)
            //
            //   2. Treat them as live and intact but unknown
            //      (give them all threshold 0, voting for self only)
            //
            //   2. Treat them as live and byzantine / inconsistent
            //      (remove them from qset as we're doing here _and_ reduce
            //       threshold of any dependents)
            //
            // We expect the set of circumstances in which we have nodes with
            // null qsets is most likely just the nodes being offline, so for
            // the diagnostic purposes this checker is serving, #1 is the best
            // approximation. The tests referring to "null qsets" differentiate
            // these cases.
            CLOG_DEBUG(SCP, "Depending on node with missing QSet: {}",
                       mCfg.toShortString(v));
        }
        else
        {
            nodeBits.set(i->second);
        }
    }
    QGraph inner;
    inner.reserve(sqs.innerSets.size());
    for (auto const& i : sqs.innerSets)
    {
        inner.emplace_back(convertSCPQuorumSet(i));
    }
    return QBitSet(threshold, nodeBits, inner);
}

void
QuorumIntersectionCheckerImpl::buildGraph(QuorumTracker::QuorumMap const& qmap)
{
    mPubKeyBitNums.clear();
    mBitNumPubKeys.clear();
    mGraph.clear();

    for (auto const& pair : qmap)
    {
        if (pair.second.mQuorumSet)
        {
            size_t n = mBitNumPubKeys.size();
            mPubKeyBitNums.insert(std::make_pair(pair.first, n));
            mBitNumPubKeys.emplace_back(pair.first);
        }
        else
        {
            CLOG_DEBUG(SCP, "Node with missing QSet: {}",
                       mCfg.toShortString(pair.first));
        }
    }

    for (auto const& pair : qmap)
    {
        if (pair.second.mQuorumSet)
        {
            auto i = mPubKeyBitNums.find(pair.first);
            assert(i != mPubKeyBitNums.end());
            auto nodeNum = i->second;
            assert(nodeNum == mGraph.size());
            auto qb = convertSCPQuorumSet(*(pair.second.mQuorumSet));
            qb.log();
            mGraph.emplace_back(qb);
        }
    }
    mStats.mTotalNodes = mPubKeyBitNums.size();
}

void
QuorumIntersectionCheckerImpl::buildSCCs()
{
    mTSC.calculateSCCs();
    mStats.mNumSCCs = mTSC.mSCCs.size();
}

std::string
QuorumIntersectionCheckerImpl::nodeName(size_t node) const
{
    return mCfg.toShortString(mBitNumPubKeys.at(node));
}

bool
QuorumIntersectionCheckerImpl::networkEnjoysQuorumIntersection() const
{
    size_t nNodes = mPubKeyBitNums.size();
    if (!mQuiet)
    {
        CLOG_INFO(SCP, "Calculating {}-node network quorum intersection",
                  nNodes);
    }

    // First stage: do a single pass over the SCCs searching for one with a
    // quorum (on which to focus second stage enumeration); also note and bypass
    // second stage exhaustive scan if there are _two_ such SCCs with quorums,
    // as they necessarily contain disjoint min-quorums.
    bool foundDisjoint = false;
    BitSet scanSCC;
    for (auto const& scc : mTSC.mSCCs)
    {
        auto q = contractToMaximalQuorum(scc);
        if (!q.empty())
        {
            if (scanSCC.empty())
            {
                // This is the first SCC with a quorum, we'll make it the
                // scan SCC.
                scanSCC = scc;
                mStats.mScanSCCSize = scanSCC.count();
                CLOG_DEBUG(SCP, "Found scan SCC: {}", scc);
                CLOG_DEBUG(SCP, "Containing quorum: {}", q);
                for (size_t i = 0; scanSCC.nextSet(i); ++i)
                {
                    CLOG_DEBUG(SCP, "SCC node to scan: {}", nodeName(i));
                }
            }
            else
            {
                CLOG_DEBUG(SCP, "Found extra SCC: {}", scc);
                CLOG_DEBUG(SCP, "Containing quorum: {}", q);
                noteFoundDisjointQuorums(contractToMaximalQuorum(scanSCC), q);
                foundDisjoint = true;
                break;
            }
        }
        else
        {
            CLOG_DEBUG(SCP, "SCC contains no quorums = {}", scc);
            for (size_t i = 0; scc.nextSet(i); ++i)
            {
                CLOG_DEBUG(SCP, "Node outside scan-SCC: {}", nodeName(i));
            }
        }
    }

    if (scanSCC.empty())
    {
        // We vacuously "enjoy quorum intersection" if there are no quorums,
        // though this is probably enough of a potential problem itself that
        // it's worth warning about.
        if (!mQuiet)
        {
            CLOG_WARNING(SCP, "No quorums found in any SCC "
                              "(possible network halt)");
        }
        return true;
    }

    // Second stage: scan the scan-SCC powerset, potentially expensive.
    if (!foundDisjoint)
    {
        BitSet committed;
        BitSet remaining = scanSCC;
        MinQuorumEnumerator mqe(committed, remaining, scanSCC, *this);
        foundDisjoint = mqe.anyMinQuorumHasDisjointQuorum();
        mStats.log();
    }
    return !foundDisjoint;
}

bool
pointsToCandidate(SCPQuorumSet const& p, PublicKey const& candidate)
{
    for (auto const& k : p.validators)
    {
        if (k == candidate)
        {
            return true;
        }
    }
    for (auto const& i : p.innerSets)
    {
        if (pointsToCandidate(i, candidate))
        {
            return true;
        }
    }
    return false;
}

void
findCriticalityCandidates(SCPQuorumSet const& p,
                          std::set<std::set<PublicKey>>& candidates, bool root)
{
    // Make a singleton-set for every validator, always.
    for (auto const& k : p.validators)
    {
        std::set<PublicKey> singleton{k};
        candidates.insert(singleton);
    }

    // Not-root and no-innerSets => P is a leaf group;
    // record it!
    if (!root && p.innerSets.empty())
    {
        std::set<PublicKey> inner;
        for (auto const& k : p.validators)
        {
            inner.insert(k);
        }
        candidates.insert(inner);
    }

    // Scan innerSets recursively.
    for (auto const& i : p.innerSets)
    {
        findCriticalityCandidates(i, candidates, false);
    }
}

std::string
groupString(Config const& cfg, std::set<PublicKey> const& group)
{
    std::ostringstream out;
    bool first = true;
    out << '[';
    for (auto const& k : group)
    {
        if (!first)
        {
            out << ", ";
        }
        first = false;
        out << cfg.toShortString(k);
    }
    out << ']';
    return out.str();
}
}

namespace stellar
{
std::shared_ptr<QuorumIntersectionChecker>
QuorumIntersectionChecker::create(QuorumTracker::QuorumMap const& qmap,
                                  Config const& cfg,
                                  std::atomic<bool>& interruptFlag, bool quiet)
{
    return std::make_shared<QuorumIntersectionCheckerImpl>(
        qmap, cfg, interruptFlag, quiet);
}

std::set<std::set<PublicKey>>
QuorumIntersectionChecker::getIntersectionCriticalGroups(
    stellar::QuorumTracker::QuorumMap const& qmap, stellar::Config const& cfg,
    std::atomic<bool>& interruptFlag)
{
    // We're going to search for "intersection-critical" groups, by considering
    // each SCPQuorumSet S that (a) has no innerSets of its own and (b) occurs
    // as an innerSet of anyone in the qmap, and evaluating whether the group of
    // validators in S can cause the network to split if they're reconfigured to
    // be "fickle". Any group whose fickleness can cause a split is considered
    // "intersection-critical". We also consider every individual node as a
    // singleton group, though these are typically unlikely to cause a split
    // on their own when made-fickle.
    //
    // Specifically a group is "fickle" if its threshold is 2 and the set of
    // validators it has to choose from has two innerSets: one that's the group
    // itself, and one that contains everyone that depends on any member of the
    // group. In other words the group will "go along with anyone". This is an
    // overapproximation of "bad configuration": the group's still online and
    // behaving correctly, but someone really messed up its configuration.
    //
    // This is a less-dramatic (and more likely) behaviour than true Byzantine
    // failure. To model the risk of Byzantine nodes we'd remove the nodes
    // entirely and reduce the thresholds of nodes depending on them,
    // effectively making the Byzantine node "voting different ways in different
    // quorums", appearing to be in two separate quorums while not counting as
    // an intersection. A merely _fickle_ node will participate in any quorum
    // that asks, but still counts as an intersecting member of any two quorums
    // it's a member of.

    std::set<std::set<PublicKey>> candidates;
    std::set<std::set<PublicKey>> critical;
    QuorumTracker::QuorumMap test_qmap(qmap);

    for (auto const& k : qmap)
    {
        if (k.second.mQuorumSet)
        {
            findCriticalityCandidates(*(k.second.mQuorumSet), candidates, true);
        }
    }

    CLOG_INFO(SCP, "Examining {} node groups for intersection-criticality",
              candidates.size());

    for (auto const& group : candidates)
    {
        // Every member of the group will share the same fickle qset.
        auto fickleQSet = std::make_shared<SCPQuorumSet>();

        // The fickle qset has 2 innerSets: self and others.
        SCPQuorumSet groupQSet;
        SCPQuorumSet pointsToGroupQSet;

        for (auto const& k : group)
        {
            groupQSet.validators.emplace_back(k);
        }
        groupQSet.threshold = static_cast<uint32>(group.size());

        std::set<PublicKey> pointsToGroup;
        for (PublicKey const& candidate : group)
        {
            for (auto const& d : qmap)
            {
                if (group.find(d.first) == group.end() && d.second.mQuorumSet &&
                    pointsToCandidate(*(d.second.mQuorumSet), candidate))
                {
                    pointsToGroup.insert(d.first);
                }
            }
        }
        for (auto const& p : pointsToGroup)
        {
            pointsToGroupQSet.validators.emplace_back(p);
        }
        pointsToGroupQSet.threshold = 1;

        fickleQSet->innerSets.emplace_back(std::move(groupQSet));
        fickleQSet->innerSets.emplace_back(std::move(pointsToGroupQSet));
        fickleQSet->threshold = 2;

        // Install the fickle qset in every member of the group.
        for (auto const& candidate : group)
        {
            test_qmap[candidate] = QuorumTracker::NodeInfo{fickleQSet, 0};
        }

        // Check to see if this modified config is vulnerable to splitting.
        auto checker =
            QuorumIntersectionChecker::create(test_qmap, cfg, interruptFlag,
                                              /*quiet=*/true);
        if (checker->networkEnjoysQuorumIntersection())
        {
            CLOG_DEBUG(SCP,
                       "group is not intersection-critical: {} (with {} "
                       "depending nodes)",
                       groupString(cfg, group), pointsToGroup.size());
        }
        else
        {
            CLOG_WARNING(
                SCP,
                "Group is intersection-critical: {} (with {} depending nodes)",
                groupString(cfg, group), pointsToGroup.size());
            critical.insert(group);
        }

        // Restore proper qsets for all group members, for next iteration.
        for (auto const& candidate : group)
        {
            test_qmap[candidate] = qmap.find(candidate)->second;
        }
    }
    if (critical.empty())
    {
        CLOG_INFO(SCP, "No intersection-critical groups found");
    }
    else
    {
        CLOG_WARNING(SCP, "Found {} intersection-critical groups",
                     critical.size());
    }
    return critical;
}
}
