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
#include "util/XDROperators.h"
#include "util/numeric.h"
#include "xdrpp/marshal.h"
#include <Tracy.hpp>
#include <algorithm>
#include <functional>

namespace stellar
{
LocalNode::LocalNode(NodeID const& nodeID, bool isValidator,
                     SCPQuorumSet const& qSet, SCP* scp)
    : mNodeID(nodeID), mIsValidator(isValidator), mQSet(qSet), mSCP(scp)
{
    normalizeQSet(mQSet);
    auto const& scpDriver = mSCP->getDriver();
    mQSetHash = scpDriver.getHashOf({xdr::xdr_to_opaque(mQSet)});

    CLOG_INFO(SCP, "LocalNode::LocalNode@{} qSet: {}",
              KeyUtils::toShortString(mNodeID), hexAbbrev(mQSetHash));

    mSingleQSet = std::make_shared<SCPQuorumSet>(buildSingletonQSet(mNodeID));
    gSingleQSetHash = scpDriver.getHashOf({xdr::xdr_to_opaque(*mSingleQSet)});
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
    ZoneScoped;
    mQSetHash = mSCP->getDriver().getHashOf({xdr::xdr_to_opaque(qSet)});
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

SCPQuorumSetPtr
LocalNode::getSingletonQSet(NodeID const& nodeID)
{
    return std::make_shared<SCPQuorumSet>(buildSingletonQSet(nodeID));
}

bool
LocalNode::forAllNodes(SCPQuorumSet const& qset,
                       std::function<bool(NodeID const&)> proc)
{
    for (auto const& n : qset.validators)
    {
        if (!proc(n))
        {
            return false;
        }
    }
    for (auto const& q : qset.innerSets)
    {
        if (!forAllNodes(q, proc))
        {
            return false;
        }
    }
    return true;
}

uint64
LocalNode::computeWeight(uint64 m, uint64 total, uint64 threshold)
{
    uint64 res;
    assert(threshold <= total);
    bigDivide(res, m, threshold, total, ROUND_UP);
    return res;
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
            res = computeWeight(UINT64_MAX, d, n);
            return res;
        }
    }

    for (auto const& q : qset.innerSets)
    {
        uint64 leafW = getNodeWeight(nodeID, q);
        if (leafW)
        {
            res = computeWeight(leafW, d, n);
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
    return isVBlockingInternal(qSet, nodeSet);
}

bool
LocalNode::isVBlocking(SCPQuorumSet const& qSet,
                       std::map<NodeID, SCPEnvelopeWrapperPtr> const& map,
                       std::function<bool(SCPStatement const&)> const& filter)
{
    ZoneScoped;
    std::vector<NodeID> pNodes;
    for (auto const& it : map)
    {
        if (filter(it.second->getStatement()))
        {
            pNodes.push_back(it.first);
        }
    }

    return isVBlocking(qSet, pNodes);
}

bool
LocalNode::isQuorum(
    SCPQuorumSet const& qSet,
    std::map<NodeID, SCPEnvelopeWrapperPtr> const& map,
    std::function<SCPQuorumSetPtr(SCPStatement const&)> const& qfun,
    std::function<bool(SCPStatement const&)> const& filter)
{
    ZoneScoped;
    std::vector<NodeID> pNodes;
    for (auto const& it : map)
    {
        if (filter(it.second->getStatement()))
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
            auto qSetPtr = qfun(map.find(nodeID)->second->getStatement());
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
    SCPQuorumSet const& qset,
    std::map<NodeID, SCPEnvelopeWrapperPtr> const& map,
    std::function<bool(SCPStatement const&)> const& filter,
    NodeID const* excluded)
{
    std::set<NodeID> s;
    for (auto const& n : map)
    {
        if (filter(n.second->getStatement()))
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
    ZoneScoped;
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
        operator()(std::vector<NodeID> const& v1,
                   std::vector<NodeID> const& v2) const
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

Json::Value
LocalNode::toJson(SCPQuorumSet const& qSet, bool fullKeys) const
{
    return toJson(qSet, [&](PublicKey const& k) {
        return mSCP->getDriver().toStrKey(k, fullKeys);
    });
}

Json::Value
LocalNode::toJson(SCPQuorumSet const& qSet,
                  std::function<std::string(PublicKey const&)> r)
{
    Json::Value ret;
    ret["t"] = qSet.threshold;
    auto& entries = ret["v"];
    for (auto const& v : qSet.validators)
    {
        entries.append(r(v));
    }
    for (auto const& s : qSet.innerSets)
    {
        entries.append(toJson(s, r));
    }
    return ret;
}

std::string
LocalNode::to_string(SCPQuorumSet const& qSet) const
{
    Json::FastWriter fw;
    return fw.write(toJson(qSet, false));
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
