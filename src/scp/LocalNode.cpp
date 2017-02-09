// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "LocalNode.h"

#include "crypto/Hex.h"
#include "crypto/KeyUtils.h"
#include "crypto/SHA.h"
#include "lib/json/json.h"
#include "scp/QuorumSetUtils.h"
#include "util/Logging.h"
#include "util/types.h"
#include "xdrpp/marshal.h"
#include <algorithm>
#include <unordered_set>

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
    , mSCP(scp)
{
    normalizeQSet(mQSet);
    mQSetHash = sha256(xdr::xdr_to_opaque(mQSet));

    CLOG(INFO, "SCP") << "LocalNode::LocalNode"
                      << "@" << KeyUtils::toShortString(mNodeID)
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
    forAllNodesInternal(qset, [&](NodeID const& n) {
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
            bigDivide(res, UINT64_MAX, n, d, ROUND_DOWN);
            return res;
        }
    }

    for (auto const& q : qset.innerSets)
    {
        uint64 leafW = getNodeWeight(nodeID, q);
        if (leafW)
        {
            bigDivide(res, leafW, n, d, ROUND_DOWN);
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
        auto quorumFilter = [&](NodeID nodeID) -> bool {
            auto qSetPtr = qfun(map.find(nodeID)->second.statement);
            if (qSetPtr)
            {
                return isQuorumSlice(*qSetPtr, pNodes);
            }
            else
            {
                return false;
            }
        };
        auto it = std::copy_if(pNodes.begin(), pNodes.end(), fNodes.begin(),
                               quorumFilter);
        fNodes.resize(std::distance(fNodes.begin(), it));
        pNodes = fNodes;
    } while (count != pNodes.size());

    return isQuorumSlice(qSet, pNodes);
}

std::vector<NodeID>
LocalNode::findClosestVBlocking(
    SCPQuorumSet const& qset, std::map<NodeID, SCPEnvelope> const& map,
    std::function<bool(SCPStatement const&)> const& filter,
    NodeID const* excluded)
{
    std::set<NodeID> s;
    for (auto const& n : map)
    {
        if (filter(n.second.statement))
        {
            s.emplace(n.first);
        }
    }
    return findClosestVBlocking(qset, s, excluded);
}

std::vector<NodeID>
LocalNode::findClosestVBlocking(SCPQuorumSet const& qset,
                                std::set<NodeID> const& nodes,
                                NodeID const* excluded)
{
    size_t leftTillBlock =
        ((1 + qset.validators.size() + qset.innerSets.size()) - qset.threshold);

    std::vector<NodeID> res;

    // first, compute how many top level items need to be blocked
    for (auto const& validator : qset.validators)
    {
        if (!excluded || !(validator == *excluded))
        {
            auto it = nodes.find(validator);
            if (it == nodes.end())
            {
                leftTillBlock--;
                if (leftTillBlock == 0)
                {
                    // already blocked
                    return std::vector<NodeID>();
                }
            }
            else
            {
                // save this for later
                res.emplace_back(validator);
            }
        }
    }

    struct orderBySize
    {
        bool
        operator()(std::vector<NodeID> const& v1, std::vector<NodeID> const& v2)
        {
            return v1.size() < v2.size();
        }
    };

    std::multiset<std::vector<NodeID>, orderBySize> resInternals;

    for (auto const& inner : qset.innerSets)
    {
        auto v = findClosestVBlocking(inner, nodes, excluded);
        if (v.size() == 0)
        {
            leftTillBlock--;
            if (leftTillBlock == 0)
            {
                // already blocked
                return std::vector<NodeID>();
            }
        }
        else
        {
            resInternals.emplace(v);
        }
    }

    // use the top level validators to get closer
    if (res.size() > leftTillBlock)
    {
        res.resize(leftTillBlock);
    }
    leftTillBlock -= res.size();

    // use subsets to get closer, using the smallest ones first
    auto it = resInternals.begin();
    while (leftTillBlock != 0 && it != resInternals.end())
    {
        res.insert(res.end(), it->begin(), it->end());
        leftTillBlock--;
        it++;
    }

    return res;
}

void
LocalNode::toJson(SCPQuorumSet const& qSet, Json::Value& value) const
{
    value["t"] = qSet.threshold;
    auto& entries = value["v"];
    for (auto const& v : qSet.validators)
    {
        entries.append(mSCP->getDriver().toShortString(v));
    }
    for (auto const& s : qSet.innerSets)
    {
        Json::Value iV;
        toJson(s, iV);
        entries.append(iV);
    }
}

std::string
LocalNode::to_string(SCPQuorumSet const& qSet) const
{
    Json::Value v;
    toJson(qSet, v);
    Json::FastWriter fw;
    return fw.write(v);
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

SCP::TriBool
LocalNode::isNodeInQuorum(
    NodeID const& node,
    std::function<SCPQuorumSetPtr(SCPStatement const&)> const& qfun,
    std::map<NodeID, std::vector<SCPStatement const*>> const& map) const
{
    // perform a transitive search, starting with the local node
    // the order is not important, so we can use sets to keep track of the work
    std::unordered_set<NodeID> backlog;
    std::unordered_set<NodeID> visited;
    backlog.insert(mNodeID);

    SCP::TriBool res = SCP::TB_FALSE;

    while (backlog.size() != 0)
    {
        auto it = backlog.begin();
        auto c = *it;
        if (c == node)
        {
            return SCP::TB_TRUE;
        }
        backlog.erase(it);
        visited.insert(c);

        auto ite = map.find(c);
        if (ite == map.end())
        {
            // can't lookup information on this node
            res = SCP::TB_MAYBE;
            continue;
        }
        for (auto st : ite->second)
        {
            auto qset = qfun(*st);
            if (!qset)
            {
                // can't find the quorum set
                res = SCP::TB_MAYBE;
                continue;
            }
            // see if we need to explore further
            forAllNodes(*qset, [&](NodeID const& n) {
                if (visited.find(n) == visited.end())
                {
                    backlog.insert(n);
                }
            });
        }
    }
    return res;
}
}
