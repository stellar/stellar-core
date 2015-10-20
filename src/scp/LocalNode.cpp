// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "LocalNode.h"

#include "util/types.h"
#include "xdrpp/marshal.h"
#include "util/Logging.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include <algorithm>

namespace stellar
{
using xdr::operator==;
using xdr::operator<;

LocalNode::LocalNode(SecretKey const& secretKey, bool isValidator,
                     SCPQuorumSet const& qSet, SCP* scp)
    : mNodeID(secretKey.getPublicKey())
    , mSecretKey(secretKey)
    , mIsValidator(isValidator)
    , mQSet(qSet)
    , mQSetHash(sha256(xdr::xdr_to_opaque(qSet)))
    , mSCP(scp)
{
    CLOG(INFO, "SCP") << "LocalNode::LocalNode"
                      << "@" << PubKeyUtils::toShortString(mNodeID)
                      << " qSet: " << hexAbbrev(mQSetHash);

    mSingleQSet = std::make_shared<SCPQuorumSet>(buildSingletonQSet(mNodeID));
    gSingleQSetHash = sha256(xdr::xdr_to_opaque(*mSingleQSet));
}

SCPQuorumSet
LocalNode::buildSingletonQSet(NodeID const& nodeID)
{
    SCPQuorumSet qSet;
    qSet.threshold = 1;
    qSet.validators.emplace_back(nodeID);
    return qSet;
}

std::pair<bool, bool>
LocalNode::isQuorumSetSaneInternal(NodeID const& nodeID,
                                   SCPQuorumSet const& qSet)
{
    auto& v = qSet.validators;
    auto& i = qSet.innerSets;

    size_t totEntries = v.size() + i.size();
    // threshold is within the proper range
    bool wellFormed = (qSet.threshold >= 1 && qSet.threshold <= totEntries);
    bool found = false;

    if (std::find(v.begin(), v.end(), nodeID) != v.end())
    {
        found = true;
    }
    for (auto const& iSet : i)
    {
        auto r = isQuorumSetSaneInternal(nodeID, iSet);
        found = found || r.first;
        wellFormed = wellFormed && r.second;
    }
    return std::make_pair(found, wellFormed);
}

bool
LocalNode::isQuorumSetSane(NodeID const& nodeID, SCPQuorumSet const& qSet)
{
    auto res = isQuorumSetSaneInternal(nodeID, qSet);
    // it's OK for a non validating node to not have itself in its quorum set
    return (res.first || (!mIsValidator && nodeID == mNodeID)) && res.second;
}

void
LocalNode::updateQuorumSet(SCPQuorumSet const& qSet)
{
    mQSetHash = sha256(xdr::xdr_to_opaque(qSet));
    mQSet = qSet;
}

SCPQuorumSet const&
LocalNode::getQuorumSet()
{
    return mQSet;
}

Hash const&
LocalNode::getQuorumSetHash()
{
    return mQSetHash;
}

SecretKey const&
LocalNode::getSecretKey()
{
    return mSecretKey;
}

SCPQuorumSetPtr
LocalNode::getSingletonQSet(NodeID const& nodeID)
{
    return std::make_shared<SCPQuorumSet>(buildSingletonQSet(nodeID));
}
void
LocalNode::forAllNodesInternal(SCPQuorumSet const& qset,
                               std::function<void(NodeID const&)> proc)
{
    for (auto const& n : qset.validators)
    {
        proc(n);
    }
    for (auto const& q : qset.innerSets)
    {
        forAllNodesInternal(q, proc);
    }
}

// runs proc over all nodes contained in qset
void
LocalNode::forAllNodes(SCPQuorumSet const& qset,
                       std::function<void(NodeID const&)> proc)
{
    std::set<NodeID> done;
    forAllNodesInternal(qset, [&](NodeID const& n)
                        {
                            auto ins = done.insert(n);
                            if (ins.second)
                            {
                                proc(n);
                            }
                        });
}

// if a validator is repeated multiple times its weight is only the
// weight of the first occurrence
uint64
LocalNode::getNodeWeight(NodeID const& nodeID, SCPQuorumSet const& qset)
{
    uint64 n = qset.threshold;
    uint64 d = qset.innerSets.size() + qset.validators.size();
    uint64 res;

    for (auto const& qsetNode : qset.validators)
    {
        if (qsetNode == nodeID)
        {
            bigDivide(res, UINT64_MAX, n, d);
            return res;
        }
    }

    for (auto const& q : qset.innerSets)
    {
        uint64 leafW = getNodeWeight(nodeID, q);
        if (leafW)
        {
            bigDivide(res, leafW, n, d);
            return res;
        }
    }

    return 0;
}

bool
LocalNode::isQuorumSliceInternal(SCPQuorumSet const& qset,
                                 std::vector<NodeID> const& nodeSet)
{
    uint32 thresholdLeft = qset.threshold;
    for (auto const& validator : qset.validators)
    {
        auto it = std::find(nodeSet.begin(), nodeSet.end(), validator);
        if (it != nodeSet.end())
        {
            thresholdLeft--;
            if (thresholdLeft <= 0)
            {
                return true;
            }
        }
    }

    for (auto const& inner : qset.innerSets)
    {
        if (isQuorumSliceInternal(inner, nodeSet))
        {
            thresholdLeft--;
            if (thresholdLeft <= 0)
            {
                return true;
            }
        }
    }
    return false;
}

bool
LocalNode::isQuorumSlice(SCPQuorumSet const& qSet,
                         std::vector<NodeID> const& nodeSet)
{
    CLOG(TRACE, "SCP") << "LocalNode::isQuorumSlice"
                       << " nodeSet.size: " << nodeSet.size();

    return isQuorumSliceInternal(qSet, nodeSet);
}

// called recursively
bool
LocalNode::isVBlockingInternal(SCPQuorumSet const& qset,
                               std::vector<NodeID> const& nodeSet)
{
    // There is no v-blocking set for {\empty}
    if (qset.threshold == 0)
    {
        return false;
    }

    int leftTillBlock =
        (int)((1 + qset.validators.size() + qset.innerSets.size()) -
              qset.threshold);

    for (auto const& validator : qset.validators)
    {
        auto it = std::find(nodeSet.begin(), nodeSet.end(), validator);
        if (it != nodeSet.end())
        {
            leftTillBlock--;
            if (leftTillBlock <= 0)
            {
                return true;
            }
        }
    }
    for (auto const& inner : qset.innerSets)
    {
        if (isVBlockingInternal(inner, nodeSet))
        {
            leftTillBlock--;
            if (leftTillBlock <= 0)
            {
                return true;
            }
        }
    }

    return false;
}

bool
LocalNode::isVBlocking(SCPQuorumSet const& qSet,
                       std::vector<NodeID> const& nodeSet)
{
    CLOG(TRACE, "SCP") << "LocalNode::isVBlocking"
                       << " nodeSet.size: " << nodeSet.size();

    return isVBlockingInternal(qSet, nodeSet);
}

bool
LocalNode::isVBlocking(SCPQuorumSet const& qSet,
                       std::map<NodeID, SCPEnvelope> const& map,
                       std::function<bool(SCPStatement const&)> const& filter)
{
    std::vector<NodeID> pNodes;
    for (auto const& it : map)
    {
        if (filter(it.second.statement))
        {
            pNodes.push_back(it.first);
        }
    }

    return isVBlocking(qSet, pNodes);
}

bool
LocalNode::isQuorum(
    SCPQuorumSet const& qSet, std::map<NodeID, SCPEnvelope> const& map,
    std::function<SCPQuorumSetPtr(SCPStatement const&)> const& qfun,
    std::function<bool(SCPStatement const&)> const& filter)
{
    std::vector<NodeID> pNodes;
    for (auto const& it : map)
    {
        if (filter(it.second.statement))
        {
            pNodes.push_back(it.first);
        }
    }

    size_t count = 0;
    do
    {
        count = pNodes.size();
        std::vector<NodeID> fNodes(pNodes.size());
        auto quorumFilter = [&](NodeID nodeID) -> bool
        {
            return isQuorumSlice(*qfun(map.find(nodeID)->second.statement),
                                 pNodes);
        };
        auto it = std::copy_if(pNodes.begin(), pNodes.end(), fNodes.begin(),
                               quorumFilter);
        fNodes.resize(std::distance(fNodes.begin(), it));
        pNodes = fNodes;
    } while (count != pNodes.size());

    return isQuorumSlice(qSet, pNodes);
}

NodeID const&
LocalNode::getNodeID()
{
    return mNodeID;
}

bool
LocalNode::isValidator()
{
    return mIsValidator;
}
}
