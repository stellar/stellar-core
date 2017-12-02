#include "history/InferredQuorum.h"
#include "crypto/SHA.h"
#include "util/BitsetEnumerator.h"
#include "util/Logging.h"
#include "xdrpp/marshal.h"
#include <fstream>
#include <sstream>

namespace stellar
{

void
InferredQuorum::noteSCPHistory(SCPHistoryEntry const& hist)
{
    for (auto const& qset : hist.v0().quorumSets)
    {
        noteQset(qset);
    }
    for (auto const& msg : hist.v0().ledgerMessages.messages)
    {
        auto pk = msg.statement.nodeID;
        notePubKey(pk);
        auto const& pledges = msg.statement.pledges;
        switch (pledges.type())
        {
        case SCP_ST_PREPARE:
            noteQsetHash(pk, pledges.prepare().quorumSetHash);
            break;
        case SCP_ST_CONFIRM:
            noteQsetHash(pk, pledges.confirm().quorumSetHash);
            break;
        case SCP_ST_EXTERNALIZE:
            noteQsetHash(pk, pledges.externalize().commitQuorumSetHash);
            break;
        case SCP_ST_NOMINATE:
            noteQsetHash(pk, pledges.nominate().quorumSetHash);
            break;
        }
    }
}

void
InferredQuorum::noteQsetHash(PublicKey const& pk, Hash const& qsetHash)
{
    auto range = mQsetHashes.equal_range(pk);
    for (auto i = range.first; i != range.second; ++i)
    {
        if (i->second == qsetHash)
        {
            // Already noted, quit now.
            return;
        }
    }
    mQsetHashes.insert(std::make_pair(pk, qsetHash));
}

void
InferredQuorum::noteQset(SCPQuorumSet const& qset)
{
    Hash qSetHash = sha256(xdr::xdr_to_opaque(qset));
    if (mQsets.find(qSetHash) == mQsets.end())
    {
        mQsets.insert(std::make_pair(qSetHash, qset));
    }
    for (auto const& pk : qset.validators)
    {
        notePubKey(pk);
    }
    for (auto const& inner : qset.innerSets)
    {
        noteQset(inner);
    }
}

void
InferredQuorum::notePubKey(PublicKey const& pk)
{
    mPubKeys[pk]++;
}

static std::shared_ptr<BitsetEnumerator>
makeQsetEnumerator(SCPQuorumSet const& qset,
                   std::unordered_map<PublicKey, size_t> const& nodeNumbers)
{
    std::vector<std::shared_ptr<BitsetEnumerator>> innerEnums;
    for (auto const& v : qset.validators)
    {
        auto i = nodeNumbers.find(v);
        assert(i != nodeNumbers.end());
        innerEnums.push_back(ConstantEnumerator::bitNumber(i->second));
    }
    for (auto const& s : qset.innerSets)
    {
        innerEnums.push_back(makeQsetEnumerator(s, nodeNumbers));
    }
    return std::make_shared<SelectionEnumerator>(
        std::make_shared<PermutationEnumerator>(qset.threshold,
                                                innerEnums.size()),
        innerEnums);
}

static std::shared_ptr<BitsetEnumerator>
makeSliceEnumerator(InferredQuorum const& iq, PublicKey const& pk,
                    std::unordered_map<PublicKey, size_t> const& nodeNumbers)
{
    // Enumerating a slice is the cartesian product enumeration of a
    // constant enumerator (for the node itself) and a selection enumerator
    // that does n-of-k for its validators and subqsets.
    std::vector<std::shared_ptr<BitsetEnumerator>> innerEnums;

    auto i = nodeNumbers.find(pk);
    assert(i != nodeNumbers.end());
    innerEnums.push_back(ConstantEnumerator::bitNumber(i->second));

    auto qsh = iq.mQsetHashes.find(pk);
    assert(qsh != iq.mQsetHashes.end());

    auto qs = iq.mQsets.find(qsh->second);
    assert(qs != iq.mQsets.end());

    innerEnums.push_back(makeQsetEnumerator(qs->second, nodeNumbers));
    return std::make_shared<CartesianProductEnumerator>(innerEnums);
}

static bool isQuorum(std::bitset<64> const& q, InferredQuorum const& iq,
                     std::unordered_map<PublicKey, size_t> const& nodeNumbers,
                     std::vector<PublicKey> const& revNodeNumbers)
{
    for (size_t i = 0; i < q.size(); ++i)
    {
        if (q.test(i))
        {
            auto e = makeSliceEnumerator(iq, revNodeNumbers.at(i), nodeNumbers);
            if (!e)
            {
                return false;
            }
            bool containsSliceForE = false;
            while (*e)
            {
                // If we find _any_ slice in e's slices that
                // is covered by q, we're good.
                if ((q | **e) == q)
                {
                    containsSliceForE = true;
                    break;
                }
                ++(*e);
            }
            if (!containsSliceForE)
            {
                return false;
            }
        }
    }
    return true;
}

bool
InferredQuorum::checkQuorumIntersection(Config const& cfg) const
{
    // Definition (quorum). A set of nodes U ⊆ V in FBAS ⟨V,Q⟩ is a quorum
    // iff U =/= ∅ and U contains a slice for each member -- i.e., ∀ v ∈ U,
    // ∃ q ∈ Q(v) such that q ⊆ U.
    //
    // Definition (quorum intersection). An FBAS enjoys quorum intersection
    // iff any two of its quorums share a node—i.e., for all quorums U1 and
    // U2, U1 ∩ U2 =/= ∅.

    // Assign a bit-number to each node
    std::unordered_map<PublicKey, size_t> nodeNumbers;
    std::vector<PublicKey> revNodeNumbers;
    for (auto const& n : mPubKeys)
    {
        nodeNumbers.insert(std::make_pair(n.first, nodeNumbers.size()));
        revNodeNumbers.push_back(n.first);
    }

    // We're (only) going to scan the powerset of the nodes we _have_ qsets
    // for, which might be significantly fewer than the total set of nodes;
    // we can't really tell how nodes we don't have qsets for will behave
    // in a network; we exclude them.
    std::unordered_set<size_t> nodesWithQsets;
    std::vector<std::shared_ptr<BitsetEnumerator>> nodeEnumerators;
    for (auto const& n : mQsetHashes)
    {
        assert(mQsets.find(n.second) != mQsets.end());
        auto i = nodeNumbers.find(n.first);
        assert(i != nodeNumbers.end());
        nodesWithQsets.insert(i->second);
    }
    for (auto nwq : nodesWithQsets)
    {
        nodeEnumerators.push_back(ConstantEnumerator::bitNumber(nwq));
    }

    // Build an enumerator for the powerset of the nodes we have qsets for;
    // this will thus return _candidate_ quorums, each of which we'll check
    // for quorum-ness.
    SelectionEnumerator quorumCandidateEnumerator(
        std::make_shared<PowersetEnumerator>(nodeEnumerators.size()),
        nodeEnumerators);

    assert(nodeEnumerators.size() < 64);
    uint64_t lim = 1ULL << nodeEnumerators.size();
    CLOG(INFO, "History") << "Scanning: " << lim
                          << " possible node subsets (of "
                          << nodeEnumerators.size() << " nodes with qsets)";

    // Enumerate all the quorums, de-duplicating into a hashset
    std::unordered_set<uint64_t> allQuorums;
    while (quorumCandidateEnumerator)
    {
        auto bv = *quorumCandidateEnumerator;
        if (isQuorum(bv, *this, nodeNumbers, revNodeNumbers))
        {
            CLOG(INFO, "History") << "Quorum: " << bv;
            allQuorums.insert(bv.to_ullong());
        }
        ++quorumCandidateEnumerator;
    }

    // Report what we found.
    for (auto const& pk : mPubKeys)
    {
        if (mQsetHashes.find(pk.first) == mQsetHashes.end())
        {
            CLOG(WARNING, "History")
                << "Node without qset: " << cfg.toShortString(pk.first);
        }
    }
    CLOG(INFO, "History") << "Found " << nodeNumbers.size() << " nodes total";
    CLOG(INFO, "History") << "Found " << nodeEnumerators.size()
                          << " nodes with qsets";
    CLOG(INFO, "History") << "Found " << allQuorums.size() << " quorums";

    bool allOk = true;
    for (auto const& q : allQuorums)
    {
        for (auto const& v : allQuorums)
        {
            if (q != v)
            {
                if (!(q & v))
                {
                    allOk = false;
                    CLOG(WARNING, "History")
                        << "Warning: found pair of non-intersecting quorums";
                    CLOG(WARNING, "History") << std::bitset<64>(q);
                    CLOG(WARNING, "History") << "vs.";
                    CLOG(WARNING, "History") << std::bitset<64>(v);
                }
            }
        }
    }

    if (allOk)
    {
        CLOG(INFO, "History") << "Network of " << nodeEnumerators.size()
                              << " nodes enjoys quorum intersection: ";
    }
    else
    {
        CLOG(WARNING, "History")
            << "Network of " << nodeEnumerators.size()
            << " nodes DOES NOT enjoy quorum intersection: ";
    }
    for (auto n : nodesWithQsets)
    {
        auto isAlias = false;
        auto name = cfg.toStrKey(revNodeNumbers.at(n), isAlias);
        if (allOk)
        {
            CLOG(INFO, "History")
                << "  \"" << (isAlias ? "$" : "") << name << '"';
        }
        else
        {
            CLOG(WARNING, "History")
                << "  \"" << (isAlias ? "$" : "") << name << '"';
        }
    }
    return allOk;
}

std::string
InferredQuorum::toString(Config const& cfg) const
{
    std::ostringstream out;

    // By default we will emit only those keys involved in half or
    // more of the quorums we've observed them in. This could be made
    // more clever.
    size_t thresh = 0;
    for (auto const& pair : mPubKeys)
    {
        thresh = pair.second > thresh ? pair.second : thresh;
    }
    thresh >>= 2;

    for (auto const& pair : mPubKeys)
    {
        auto isAlias = false;
        auto name = cfg.toStrKey(pair.first, isAlias);
        if (pair.second < thresh)
        {
            out << "# skipping unreliable "
                << "(" << pair.second << "/" << thresh << ") node: " << '"'
                << (isAlias ? "$" : "") << name << '"' << std::endl;
        }
    }

    out << "[QUORUM_SET]" << std::endl;
    out << "[" << std::endl;
    auto first = true;
    for (auto const& pair : mPubKeys)
    {
        if (pair.second < thresh)
        {
            continue;
        }
        auto isAlias = false;
        auto name = cfg.toStrKey(pair.first, isAlias);
        if (first)
        {
            first = false;
        }
        else
        {
            out << "," << std::endl;
        }
        out << '"' << (isAlias ? "$" : "") << name << '"';
    }
    out << std::endl << "]" << std::endl;
    return out.str();
}

void
InferredQuorum::writeQuorumGraph(Config const& cfg, std::ostream& out) const
{
    out << "digraph {" << std::endl;
    for (auto const& pkq : mQsetHashes)
    {
        auto qp = mQsets.find(pkq.second);
        if (qp != mQsets.end())
        {
            auto src = cfg.toShortString(pkq.first);
            for (auto const& dst : qp->second.validators)
            {
                out << src << " -> " << cfg.toShortString(dst) << ";"
                    << std::endl;
            }
            for (auto const& iqs : qp->second.innerSets)
            {
                for (auto const& dst : iqs.validators)
                {
                    out << src << " -> " << cfg.toShortString(dst) << ";"
                        << std::endl;
                }
            }
        }
    }
    out << "}" << std::endl;
}
}
