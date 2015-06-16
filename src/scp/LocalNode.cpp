// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "LocalNode.h"

#include <cassert>
#include "util/types.h"
#include "xdrpp/marshal.h"
#include "util/Logging.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"

namespace stellar
{

LocalNode::LocalNode(SecretKey const& secretKey, SCPQuorumSet const& qSet,
                     SCP* scp)
    : mNodeID(secretKey.getPublicKey())
    , mSecretKey(secretKey)
    , mQSet(qSet)
    , mQSetHash(sha256(xdr::xdr_to_opaque(qSet)))
    , mSCP(scp)
{
    CLOG(INFO, "SCP") << "LocalNode::LocalNode"
                      << "@" << hexAbbrev(mNodeID)
                      << " qSet: " << hexAbbrev(mQSetHash);

    mSingleQSet = std::make_shared<SCPQuorumSet>(buildSingletonQSet(mNodeID));
    gSingleQSetHash = sha256(xdr::xdr_to_opaque(*mSingleQSet));
}

SCPQuorumSet
LocalNode::buildSingletonQSet(uint256 const& nodeID)
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
LocalNode::getSingletonQSet(uint256 const& nodeID)
{
    return std::make_shared<SCPQuorumSet>(buildSingletonQSet(nodeID));
}
void
LocalNode::forAllNodesInternal(SCPQuorumSet const& qset,
                               std::function<void(uint256 const&)> proc)
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
                       std::function<void(uint256 const&)> proc)
{
    std::set<uint256> done;
    forAllNodesInternal(qset, [&](uint256 const& n)
                        {
                            auto ins = done.insert(n);
                            if (ins.second)
                            {
                                proc(n);
                            }
                        });
}

uint64
LocalNode::getNodeWeight(uint256 const& nodeID, SCPQuorumSet const& qset)
{
    // TODO: this is a bogus implementation that has "close-enough" properties
    uint64 total = 0;
    uint64 p = 0;
    forAllNodes(qset, [&](uint256 const& n)
                {
                    total++;
                    if (n == nodeID)
                    {
                        p++;
                    }
                });
    uint64 res;
    if (!bigDivide(res, UINT64_MAX, p, total))
    {
        abort();
    }
    return res;
}

bool
LocalNode::isQuorumSliceInternal(SCPQuorumSet const& qset,
                                 std::vector<uint256> const& nodeSet)
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
                         std::vector<uint256> const& nodeSet)
{
    CLOG(DEBUG, "SCP") << "LocalNode::isQuorumSlice"
                       << " nodeSet.size: " << nodeSet.size();

    return isQuorumSliceInternal(qSet, nodeSet);
}

// called recursively
bool
LocalNode::isVBlockingInternal(SCPQuorumSet const& qset,
                               std::vector<uint256> const& nodeSet)
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
                       std::vector<uint256> const& nodeSet)
{
    CLOG(DEBUG, "SCP") << "LocalNode::isVBlocking"
                       << " nodeSet.size: " << nodeSet.size();

    return isVBlockingInternal(qSet, nodeSet);
}

bool
LocalNode::isVBlocking(
    SCPQuorumSet const& qSet, std::map<uint256, SCPStatement> const& map,
    std::function<bool(uint256 const&, SCPStatement const&)> const& filter)
{
    std::vector<uint256> pNodes;
    for (auto const& it : map)
    {
        if (filter(it.first, it.second))
        {
            pNodes.push_back(it.first);
        }
    }

    return isVBlocking(qSet, pNodes);
}

bool
LocalNode::isQuorum(
    SCPQuorumSet const& qSet, std::map<uint256, SCPStatement> const& map,
    std::function<SCPQuorumSetPtr(SCPStatement const&)> const& qfun,
    std::function<bool(uint256 const&, SCPStatement const&)> const& filter)
{
    std::vector<uint256> pNodes;
    for (auto const& it : map)
    {
        if (filter(it.first, it.second))
        {
            pNodes.push_back(it.first);
        }
    }

    size_t count = 0;
    do
    {
        count = pNodes.size();
        std::vector<uint256> fNodes(pNodes.size());
        auto quorumFilter = [&](uint256 nodeID) -> bool
        {
            return isQuorumSlice(*qfun(map.find(nodeID)->second), pNodes);
        };
        auto it = std::copy_if(pNodes.begin(), pNodes.end(), fNodes.begin(),
                               quorumFilter);
        fNodes.resize(std::distance(fNodes.begin(), it));
        pNodes = fNodes;
    } while (count != pNodes.size());

    return isQuorumSlice(qSet, pNodes);
}

uint256 const&
LocalNode::getNodeID()
{
    return mNodeID;
}
}
