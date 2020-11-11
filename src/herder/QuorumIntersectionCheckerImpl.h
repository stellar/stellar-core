#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

////////////////////////////////////////////////////////////////////////////////
// Quorum intersection checking
////////////////////////////////////////////////////////////////////////////////
//
// Algorithm thanks to Łukasz Lachowski <l.lachowski@gmail.com>, code largely
// derived from his implementation (Copyright 2018, MIT licensed).
//
//   See https://arxiv.org/pdf/1902.06493.pdf
//   and https://github.com/fixxxedpoint/quorum_intersection.git
//
// There's a fair bit of ground to cover in understanding what this algorithm is
// doing and convincing you, reader, that it's correct (as I had to convince
// myself). I've therefore (re)written it in as plain and explicit (and maybe
// over-verbose) a style as possible, to be as expository and convincing, as
// well as including this gruesomely long comment to guide you in
// reading. Please don't add anything clever unless you've got a very good
// performance reason, and even then make sure you add docs explaining what
// you've done.
//
//
// Definitions
// ===========
//
// - A network N is a set of nodes {N₀, N₁, ...}
//
// - Every node Nᵢ has an associated quorum set (or "qset") QSᵢ that is a set of
//   subsets of N.
//
// - Every element of a qset QSᵢ is called a quorum slice.
//
// - A quorum Q ⊆ N is a set satisfying: Q ≠ ∅ and Ɐ Nᵢ ∈ Q, ∃ S ∈ QSᵢ, S ⊆ Q
//
// - Equivalently in english: a quorum is a subset of the network such that for
//   each node Nᵢ in the quorum, there's some slice S in the node's quorum set
//   QSᵢ that is itself (S) also a subset of the quorum. In this case we also
//   say the quorum "contains a slice for Nᵢ" or "satisfies QSᵢ" or "satisfies
//   Nᵢ". Note that this definition is not always satisfiable: many networks
//   have no quorums.
//
// - A network N "enjoys quorum intersection" if every pair of quorums
//   intersects in at least a single node.
//
// - A minimal quorum (or min-quorum or minq) is a quorum that does not have any
//   quorum as a proper subset.
//
// - A network N and its quorum sets induce a directed graph Gₙ, in which the
//   vertices are the nodes of the network and an edge exists between any node
//   Nᵢ and Nⱼ when Nⱼ is in one of the quorum slices in QSᵢ. In other words,
//   the graph has an edge for every dependency between a node and a member
//   of one of its quorum slices.
//
// - A strongly connected component is a subset C of a network in which every
//   pair of nodes Nᵢ and Nⱼ ∈ C can reach each other by following edges Gₙ.
//
//
// Intuition
// =========
//
// We will build up an algorithm for checking quorum intersection one step at
// a time.
//
// First: assume we have as a building block the ability to check a set of nodes
// for being-a-quorum. This part isn't hard and there's code implementing it
// below (QuorumIntersectionChecker::isAQuorum, though it's impelmented in terms
// of refinement 2 so don't read ahead just yet, just trust me).
//
// We could check quorum intersection for a network N via a loop like this:
//
//  foreach subset S₁ of N:
//    if S₁ is a quorum:
//      foreach subset S₂ of N:
//        if S₂ is a quorum:
//          check S₁∩ S₂≠ ∅
//
// And that would clearly suffice! It's practically the definition, rewritten in
// code, and in fact this is how we initially wrote quorum intersection
// checking. The only problem is that it is very slow: it enumerates the
// powerset P(N) of all subsets of the network, and even does so again, in a
// nested loop. This means it takes O(2ⁿ)² time. Bad.
//
// But the intuition is fine and the algorithm we wind up with is "just" a lot
// of refinements to that intuition. We're going to explore conceptually the
// same search space, just with a bunch of simplifying assumptions that let us
// skip most of it because we've convinced ourselves there are no "hits" in the
// parts we skip.
//
//
// Refinement 1: Switch to checking the complement
// ===============================================
//
// The nested testing and intersection loop above can be rewritten the following
// way while preserving logical equivalence:
//
// foreach subset S₁ of N:
//   if S₁ is a quorum:
//     for each subset S₂ of N \ S₁:
//       check that S₂ is not a quorum
//
// That is, any subset S₂ of the complement of S₁ is (by definition)
// non-intersecting with S₁ and therefore needs to be not-a-quorum if we want to
// enjoy quorum intersection. This will go a bit faster than the full cartesian
// product space of the first algorithm because as S₁ expands, N \ S₁ contracts,
// but the real improvement in complexity shows up if we introduce another
// building block (which is in fact how we implement isAQuorum anyways).
//
//
// Refinement 2: Switch to contracting sets to maximal quorums
// ===========================================================
//
// Given some set S ⊆ N, we can contract S to a maximal quorum Q ⊆ S (if it
// exists) in reasonably quick time: for each node Nᵢ ∈ S, remove Nᵢ from S if S
// does not satisfy QSᵢ, and keep iterating this procedure until it reaches a
// fixpoint. The fixpoint will either be empty (in which case S contained no
// quorum) or the largest quorum contained within S. This runs in time linear
// with the size of S (times at-worst linear-time slice checking, so maybe
// quadratic; but still far better than the exponential time of the powerset!)
//
// Where this is useful is that the algorithm from refinement 1 can now be
// adapted to seek quorums directly in the complement, rather than enumerating
// subsets of the complement:
//
// foreach subset S₁ of N:
//   if S₁ is a quorum:
//     check that (N \ S₁) contracts to the empty set (i.e. has no quorum)
//
// This frees us from the cartesian product of the powersets. Good! But we're
// still exploring the powerset in the outer loop.
//
//
// Refinement 3: Enumerate from bottom up
// ======================================
//
// We haven't mentioned yet specifically how we're going to enumerate the
// powerset P(N), and it turns out one order is especially useful for our
// purposes: "bottom up", starting from 1-element sets and expanding to
// 2-element sets, then 3-element sets, and so forth.
//
// In particular we're going to pick a recursive enumeration that's based on two
// sets: C (for "committed") and R (for "remaining"). We start with C = ∅ and R
// = N. The set C at each call is the current set we're expanding, bottom-up;
// the set R is the set we're going to draw expansions from. We pass C and R to
// the following recursive enumerator:
//
//     def enumerate(C, R):
//         fail if C is a quorum and (N \ C) has a quorum
//         return if R = ∅
//         pick some node Nᵢ ∈ R
//         call enumerate(C, R \ {Nᵢ})
//         call enumerate(C ∪ {Nᵢ}, R \ {Nᵢ})
//
// Any activation of this procedure is logically enumerating the powerset of
// some "perimeter set" P = C ∪ R (initially = N), but it's doing so by
// recursively dividing the perimeter in two: picking some arbitrary Nᵢ in R,
// and in the first recursive call enumerating the subsets of P that exclude Nᵢ,
// then in the second recursive call enumerating the subsets of P that include
// Nᵢ.
//
// The reason this order is useful is that it organizes the search into a
// branching tree of recursive calls that each tell us enough about their
// pending callees that we can trim some branches of that tree without actually
// calling them. The next several refinements are adding such "early exits",
// that trim the search tree.
//
//
// Refinement 4: Only scan half the space (early exit #1)
// ======================================================
//
// The first early exit is easy: just stop expanding when you get half-way
// through the space (or precisely: at sets larger than MAXSZ = #N/2)
// because the problem is symmetric: any potential failing subset C with size
// greater than MAXSZ discovered in the branching subtree ahead will satisfy
//
//     C is a quorum and (N \ C) has a quorum
//
// and if such a C exists, it will also be discovered by some other branch of
// the search tree scanning the complement, that is enumerating subsets of (N \
// C) which has size less than MAXSZ. So there's no point looking at the
// bigger-than-half subsets in detail. So we add an exit:
//
//     def enumerate(C, R):
//         return if #C > MAXSZ                              ← new early exit
//         fail if C is a quorum and (N \ C) has a quorum
//         return if R = ∅
//         pick some node Nᵢ ∈ R
//         call enumerate(C, R \ {Nᵢ})
//         call enumerate(C ∪ {Nᵢ}, R \ {Nᵢ})
//
//
// Refinement 5: Look ahead for possible quorums (early exit #2.1 and 2.2)
// =======================================================================
//
// The second early exit is similarly simple: after we've checked C for quorum
// and before recursing into the next two branches, look at the "perimeter set"
// P = C ∪ R that defines the space in which the two branches will be exploring
// and check (using the contraction function) to see if there are any quorums in
// that space at all. And even more specifically, if any such (maximal) quorum
// is an extension (superset-or-equal) of C, since it will need to be if we're
// going to enumerate it in either of the remaining branches.
//
// If not, there's no point searching for specific quorums and their
// complements, and we can return early. We add the following early exits:
//
//     def enumerate(C, R):
//         return if #C > MAXSZ
//         fail if C is a quorum and (N \ C) has a quorum
//         return if R = ∅
//         return if (C ∪ R) has no quorum or                ← new early exits
//             its maximal quorum isn't a superset of C
//         pick some node Nᵢ ∈ R
//         call enumerate(C, R \ {Nᵢ})
//         call enumerate(C ∪ {Nᵢ}, R \ {Nᵢ})
//
//
// Refinement 6: Minimal quorums only (early exits #3.1 and 3.2)
// =============================================================
//
// So far we've been treating all quorums as equally concerning, but there is a
// key relationship we can exploit when exploring the powerset bottom-up:
// that some quorums are contained inside of other quorums.
//
// It turns out that a quorum that does not contain other quorums – a so-called
// min-quorum or minq – is sufficiently powerful to serve as a sort of
// boundary for trimming the search space. In particular, this
// possibly-surprising theorem holds:
//
//     A network enjoys quorum intersection iff every pair of minqs intersects.
//
// That is, we can redefine (while preserving equivalence) the enumeration task
// from full quorums and their complements to minqs and their complements, which
// (as we'll see) gives us another early exit. First we need to prove this
// theorem.
//
//     Forward implication: trivial. Quorum intersection is defined as "any two
//     quorums intersect". Two minqs are quorums, so they intersect by
//     assumption.
//
//     Reverse implication: by contradiction. Assume all pairs of minqs
//     intersect but some pair of quorums Q1 and Q2 do not intersect. Q1 and Q2
//     may or may not be minqs themselves, so 4 cases that all end with subsets
//     of Q1 intersecting Q2, contradicting assumption:
//
//       - If Q1 and Q2 are minqs, they intersect by assumption.
//
//       - (Two symmetric cases) If Qi is a minq and Qj is not for i != j, Qj
//         has a minq Mj inside itself (by definition of being a non-minq) and
//         Mj intersects Qi by assumption that all minqs intersect.
//
//       - If neither is a minq then each contain minqs M1 and M2 and those
//         intersect by assumption.
//
// So what is this good for? If we treat our problem as searching for minqs
// rather than more general quorums, we can do an early return in the
// enumeration any time we encounter any quorum at all: we're growing bottom-up
// from sets to supersets, but no superset of a quorum is a minq, so once we're
// at a committed set C that's a quorum there's no need to look in any further
// sub-branches of the search enlarging C: there are no more minqs down those
// branches.
//
// So we test for quorum-ness, and then for minq-ness (checking the
// complement for a non-intersecting quorum if so), and then return early in
// either case:
//
//     def enumerate(C, R):
//         return if #C > MAXSZ
//         if C is a quorum:
//             fail if C is a minq and (N \ C) has a quorum
//             return                                         ← new early exit
//         return if R = ∅
//         return if (C ∪ R) has no quorum or
//             its maximal quorum isn't a superset of C
//         pick some node Nᵢ ∈ R
//         call enumerate(C, R \ {Nᵢ})
//         call enumerate(C ∪ {Nᵢ}, R \ {Nᵢ})
//
//
// Refinement 7: heuristic node selection
// ======================================
//
// Observe above that we pick "some node Nᵢ ∈ R" but don't say how. It turns out
// that the node we choose has a dramatic effect on the amount of search space
// explored. Picking right means we get to an early exit (or failure) ASAP;
// picking wrong means we double the search without hitting any early exits in
// the next layer of the recursion.
//
// There's no good single answer for the best next-node to divide on; probably
// knowing this would be equivalent to solving the problem anyways. But we can
// use some heuristics. I've explored several and not come up with any better
// (empirically) than in Lachowski's initial code, so we stick with that
// heuristic here: we pick the node with the highest indegree, when looking at
// the subgraph of the remaining nodes R and edges between them. If there are
// multiple such nodes we pick among them randomly. This seems to favor
// discovering participants in quorums early, taking the quorum-related early
// exits ASAP.
//
//
// Refinement 8: Time for some graph theory
// ========================================
//
// There is one last step in the development here and it also involves minqs but
// not early exits in the enumeration as such. The enumerate function above is
// left alone, but we reduce the inputs to it from minqs in P(N) to minqs in
// P(M) for some M ⊆ N, reducing our search a lot. For this we need graph
// theory!
//
// The trick here is to recognize that there's a relationship between minqs and
// strongly connected components (SCCs) in the induced directed graph
// Gₙ. Specifically that the following theorem holds:
//
//     Any minq of N is entirely contained in an SCC of the induced graph Gₙ.
//
// Again it's worth pausing to prove by contradiction:
//
//     If some minq Q of N extended beyond an SCC of Gₙ, that means
//     (definitionally) that Q contains some node Nᵢ that depends on some node
//     Nⱼ that is not in the same SCC as Nᵢ. Since Q is a quorum, Q satisfies Nⱼ
//     by means of some slice SLⱼ ⊆ Q. Since Nⱼ is not in the same SCC as Nᵢ,
//     this SLⱼ must also not depend on nodes in the same SCC as Nᵢ. But if
//     that's so, then Nⱼ and SLⱼ together make up a subquorum of Q,
//     contradicting the assumption that Q is minimal.
//
// What's useful about the SCC relationship is that it means we can add a
// pre-filtering stage that analyzes Gₙ rather than P(N) and does two helpful
// things:
//
//     1. Check that only one SCC of Gₙ has quorums in it at all. If there are
//        two, then they contain a pair of disjoint minqs, and we're done
//        (quorum intersection does not hold).
//
//     2. Reduce the enumeration task to the powerset of the nodes of the SCC
//        that has quorums in it, rather than the powerset of all the nodes in
//        the graph. This typically excludes lots of nodes.
//
//
// Coda: micro-optimizations
// =========================
//
// We've finished with algorithmic improvements. All that remains is making it
// go as fast as possible. For this, we use graph and set representations that
// minimize allocation, hashing, indirection and so forth: vectors of dense
// bitsets and bitwise operations. These are not the same representations used
// elsewhere in stellar-core so there's a little work up front converting
// representations.
//
// Remaining details of the implementation are noted as we go, but the above
// explanation ought to give you a good idea what you're looking at.

#include "QuorumIntersectionChecker.h"
#include "main/Config.h"
#include "util/BitSet.h"
#include "util/RandomEvictionCache.h"
#include "xdr/Stellar-SCP.h"
#include "xdr/Stellar-types.h"

namespace
{

struct QBitSet;
using QGraph = std::vector<QBitSet>;
class QuorumIntersectionCheckerImpl;

// A QBitSet is the "fast" representation of a SCPQuorumSet. It includes both a
// BitSet of its own nodes and a set of innerSets, along with a "successors"
// BitSet that contains the union of all the bits set in the own nodes or
// innerSets, for fast successor-testing.
struct QBitSet
{
    const uint32_t mThreshold;
    const BitSet mNodes;
    const QGraph mInnerSets;

    // Union of mNodes and i.mAllSuccessors for i in mInnerSets: summarizes
    // every node that this QBitSet directly depends on.
    const BitSet mAllSuccessors;

    QBitSet(uint32_t threshold, BitSet const& nodes, QGraph const& innerSets);

    bool
    empty() const
    {
        return mThreshold == 0 && mAllSuccessors.empty();
    }

    void log(size_t indent = 0) const;

    static BitSet getSuccessors(BitSet const& nodes, QGraph const& inner);
};

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
    QGraph const& mGraph;

    TarjanSCCCalculator(QGraph const& graph);
    void calculateSCCs();
    void scc(size_t i);
};

// A MinQuorumEnumerator is responsible to scanning the powerset of the SCC
// we're considering, in a recursive bottom-up order, with a lot of early exits
// described above. Each instance of MinQuorumEnumerator represents one call in
// the recursion, and builds up to two sub-MinQuorumEnumerators for each of its
// recursive cases.
class MinQuorumEnumerator
{

    // Set of nodes "committed to" in this branch of the recurrence. In other
    // words: set of nodes that this enumerator and its children will definitely
    // include in every subset S of the powerset that they examine. This set
    // will remain the same (omitting the split node) in one child, and expand
    // (including the split node) in the other child.
    BitSet mCommitted;

    // Set of nodes that remain to be powerset-expanded in the recurrence. In
    // other words: the part of the powerset that this enumerator and its
    // children are responsible for is { committed ∪ r | r ∈ P(remaining) }.
    // This set will strictly decrease (by the split node) in both children.
    BitSet mRemaining;

    // The set (committed ∪ remaining) which is a bound on the set of nodes in
    // any set enumerated by this enumerator and its children.
    BitSet mPerimeter;

    // The initial value of mRemaining at the root of the search, representing
    // the overall SCC we're considering subsets of.
    BitSet const& mScanSCC;

    // Checker that owns us, contains state of stats, graph, etc.
    QuorumIntersectionCheckerImpl const& mQic;

    // Select the next node in mRemaining to split recursive cases between.
    size_t pickSplitNode() const;

    // Size limit for mCommitted beyond which we should stop scanning.
    size_t maxCommit() const;

  public:
    MinQuorumEnumerator(BitSet const& committed, BitSet const& remaining,
                        BitSet const& scanSCC,
                        QuorumIntersectionCheckerImpl const& qic);

    bool hasDisjointQuorum(BitSet const& nodes) const;
    bool anyMinQuorumHasDisjointQuorum();
};

// Quorum intersection checking is done by establishing a root
// QuorumIntersectionChecker on a given QuorumMap. The QuorumIntersectionChecker
// builds a QGraph of the nodes, uses TarjanSCCCalculator to calculate its SCCs,
// and then runs a MinQuorumEnumerator to recursively scan the powerset.
class QuorumIntersectionCheckerImpl : public stellar::QuorumIntersectionChecker
{

    stellar::Config const& mCfg;

    struct Stats
    {
        size_t mTotalNodes = {0};
        size_t mNumSCCs = {0};
        size_t mScanSCCSize = {0};
        size_t mCallsStarted = {0};
        size_t mFirstRecursionsTaken = {0};
        size_t mSecondRecursionsTaken = {0};
        size_t mMaxQuorumsSeen = {0};
        size_t mMinQuorumsSeen = {0};
        size_t mTerminations = {0};
        size_t mEarlyExit1s = {0};
        size_t mEarlyExit21s = {0};
        size_t mEarlyExit22s = {0};
        size_t mEarlyExit31s = {0};
        size_t mEarlyExit32s = {0};
        void log() const;
    };

    // We use our own stats and a local cached flag to control tracing because
    // using the global metrics and log-partition lookups at a fine grain
    // actually becomes problematic CPU-wise.
    mutable Stats mStats;
    bool mLogTrace;

    // When run as a subroutine of criticality-checking, we inhibit
    // INFO/ERROR/WARNING level messages.
    bool mQuiet;

    // State to capture a counterexample found during search, for later
    // reporting.
    mutable std::pair<std::vector<stellar::PublicKey>,
                      std::vector<stellar::PublicKey>>
        mPotentialSplit;

    // These are the key state of the checker: the mapping from node public keys
    // to graph node numbers, and the graph of QBitSets itself.
    std::vector<stellar::PublicKey> mBitNumPubKeys;
    std::unordered_map<stellar::PublicKey, size_t> mPubKeyBitNums;
    QGraph mGraph;

    // This is a temporary structure that's reused very often within the
    // MinQuorumEnumerators, but never reentrantly / simultaneously. So we
    // allocate it once here and let the MQEs use it to avoid hammering
    // on malloc.
    mutable std::vector<size_t> mInDegrees;

    // This just calculates SCCs, from which we extract the first one found with
    // a quorum, which (assuming no other SCCs have quorums) we'll use for the
    // remainder of the search.
    TarjanSCCCalculator mTSC;

    // Interruption flag: setting this causes the QIC / MQEs to throw
    // InterruptedException at the nearest convenient moment.
    std::atomic<bool>& mInterruptFlag;

    QBitSet convertSCPQuorumSet(stellar::SCPQuorumSet const& sqs);
    void buildGraph(stellar::QuorumTracker::QuorumMap const& qmap);
    void buildSCCs();

    bool containsQuorumSlice(BitSet const& bs, QBitSet const& qbs) const;
    bool containsQuorumSliceForNode(BitSet const& bs, size_t node) const;
    BitSet contractToMaximalQuorum(BitSet nodes) const;

    const int MAX_CACHED_QUORUMS_SIZE = 0xffff;
    mutable stellar::RandomEvictionCache<BitSet, bool, BitSet::HashFunction>
        mCachedQuorums;
    bool isAQuorum(BitSet const& nodes) const;
    bool isMinimalQuorum(BitSet const& nodes) const;
    void noteFoundDisjointQuorums(BitSet const& nodes,
                                  BitSet const& disj) const;
    std::string nodeName(size_t node) const;

    friend class MinQuorumEnumerator;

  public:
    QuorumIntersectionCheckerImpl(stellar::QuorumTracker::QuorumMap const& qmap,
                                  stellar::Config const& cfg,
                                  std::atomic<bool>& interruptFlag,
                                  bool quiet = false);
    bool networkEnjoysQuorumIntersection() const override;

    std::pair<std::vector<stellar::PublicKey>, std::vector<stellar::PublicKey>>
    getPotentialSplit() const override;
    size_t getMaxQuorumsFound() const override;
};
}
