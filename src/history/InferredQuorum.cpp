#include "history/InferredQuorum.h"
#include "crypto/SHA.h"
#include "util/Logging.h"
#include "xdrpp/marshal.h"
#include <fstream>
#include <sstream>

namespace stellar
{

InferredQuorum::InferredQuorum()
{
}

InferredQuorum::InferredQuorum(QuorumTracker::QuorumMap const& qmap)
{
    for (auto const& pair : qmap)
    {
        notePubKey(pair.first);
        if (pair.second.mQuorumSet)
        {
            noteQset(*(pair.second.mQuorumSet));
            Hash qSetHash = xdrSha256(*(pair.second.mQuorumSet));
            noteQsetHash(pair.first, qSetHash);
        }
    }
}

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
    auto& v = mQsetHashes[pk];
    for (auto const& h : v)
    {
        if (h == qsetHash)
        {
            // Already noted, quit now.
            return;
        }
    }
    v.emplace_back(qsetHash);
}

void
InferredQuorum::noteQset(SCPQuorumSet const& qset)
{
    Hash qSetHash = xdrSha256(qset);
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

std::string
InferredQuorum::toString(Config const& cfg, bool fullKeys) const
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
        auto name = cfg.toStrKey(pair.first, fullKeys);
        if (pair.second < thresh)
        {
            out << "# skipping unreliable "
                << "(" << pair.second << "/" << thresh << ") node: " << '"'
                << name << '"' << std::endl;
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
        auto name = cfg.toStrKey(pair.first, fullKeys);
        if (first)
        {
            first = false;
        }
        else
        {
            out << "," << std::endl;
        }
        out << '"' << name << '"';
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
        for (auto pkqv : pkq.second)
        {
            auto qp = mQsets.find(pkqv);
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
    }
    out << "}" << std::endl;
}

QuorumTracker::QuorumMap
InferredQuorum::getQuorumMap() const
{
    QuorumTracker::QuorumMap qm;
    for (auto const& pair : mQsetHashes)
    {
        qm[pair.first] = QuorumTracker::NodeInfo{nullptr, 0};
        for (auto i = pair.second.rbegin(); i != pair.second.rend(); ++i)
        {
            auto qi = mQsets.find(*i);
            if (qi != mQsets.end())
            {
                SCPQuorumSetPtr p = std::make_shared<SCPQuorumSet>(qi->second);
                qm[pair.first] = QuorumTracker::NodeInfo{p, 0};
                break;
            }
        }
    }
    return qm;
}
}
