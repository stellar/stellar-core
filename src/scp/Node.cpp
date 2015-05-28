// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "Node.h"

#include <cassert>
#include "xdrpp/marshal.h"
#include "util/Logging.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"



namespace stellar
{

int const Node::CACHE_SIZE = 4;

Node::Node(uint256 const& nodeID, SCP* SCP)
    : mNodeID(nodeID), mSCP(SCP)
{
}

bool 
Node::hasQuorum(SCPQuorumSet const& qset, std::vector<uint256> const& nodeSet)
{
    uint32 thresholdLeft = qset.threshold;
    for(auto const& validator : qset.validators)
    {
        auto it = std::find(nodeSet.begin(), nodeSet.end(), validator);
        if(it != nodeSet.end())
        {
            thresholdLeft--;
            if(thresholdLeft <= 0) return true;
        }
    }

    for(auto const& inner : qset.innerSets)
    {
        if(hasQuorum(inner, nodeSet))
        {
            thresholdLeft--;
            if(thresholdLeft <= 0) return true;
        }
    }
    return false;
}

bool
Node::hasQuorum(Hash const& qSetHash, std::vector<uint256> const& nodeSet)
{
    CLOG(DEBUG, "SCP") << "Node::hasQuorum"
                       << "@" << hexAbbrev(mNodeID)
                       << " qSet: " << hexAbbrev(qSetHash)
                       << " nodeSet.size: " << nodeSet.size();
    // This call can throw a `QuorumSetNotFound` if the quorumSet is unknown.
    SCPQuorumSetPtr qSet = retrieveQuorumSet(qSetHash);

    return hasQuorum(*qSet, nodeSet);
}

// called recursively
bool 
Node::isVBlocking(SCPQuorumSet const& qset, std::vector<uint256> const& nodeSet)
{
    int leftTillBlock = (int) ((qset.validators.size() + qset.innerSets.size()) - qset.threshold);

    for(auto const &validator : qset.validators)
    {
        auto it = std::find(nodeSet.begin(), nodeSet.end(), validator);
        if(it != nodeSet.end())
        {
            leftTillBlock--;
            if(leftTillBlock <= 0) return true;
        }
    }
    for(auto const& inner : qset.innerSets)
    {
        if(isVBlocking(inner, nodeSet))
        {
            leftTillBlock--;
            if(leftTillBlock <= 0) return true;
        }
    }

    return false;
}

bool
Node::isVBlocking(Hash const& qSetHash, std::vector<uint256> const& nodeSet)
{
    CLOG(DEBUG, "SCP") << "Node::isVBlocking"
                       << "@" << hexAbbrev(mNodeID)
                       << " qSet: " << hexAbbrev(qSetHash)
                       << " nodeSet.size: " << nodeSet.size();
    // This call can throw a `QuorumSetNotFound` if the quorumSet is unknown.
    SCPQuorumSetPtr qSet = retrieveQuorumSet(qSetHash);

    // There is no v-blocking set for {\empty}
    if (qSet->threshold == 0)
    {
        return false;
    }

    return isVBlocking(*qSet, nodeSet);
}

template <class T>
bool
Node::isVBlocking(Hash const& qSetHash, std::map<uint256, T> const& map,
                  std::function<bool(uint256 const&, T const&)> const& filter)
{
    std::vector<uint256> pNodes;
    for (auto const& it : map)
    {
        if (filter(it.first, it.second))
        {
            pNodes.push_back(it.first);
        }
    }

    return isVBlocking(qSetHash, pNodes);
}

template bool Node::isVBlocking<SCPStatement>(
    Hash const& qSetHash, std::map<uint256, SCPStatement> const& map,
    std::function<bool(uint256 const&, SCPStatement const&)> const& filter);

template bool Node::isVBlocking<bool>(
    Hash const& qSetHash, std::map<uint256, bool> const& map,
    std::function<bool(uint256 const&, bool const&)> const& filter);

template <class T>
bool
Node::isQuorumTransitive(
    Hash const& qSetHash, std::map<uint256, T> const& map,
    std::function<Hash(T const&)> const& qfun,
    std::function<bool(uint256 const&, T const&)> const& filter)
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
            return mSCP->getNode(nodeID)
                ->hasQuorum(qfun(map.find(nodeID)->second), pNodes);
        };
        auto it = std::copy_if(pNodes.begin(), pNodes.end(), fNodes.begin(),
                               quorumFilter);
        fNodes.resize(std::distance(fNodes.begin(), it));
        pNodes = fNodes;
    } while (count != pNodes.size());

    return hasQuorum(qSetHash, pNodes);
}

template bool Node::isQuorumTransitive<SCPStatement>(
    Hash const& qSetHash, std::map<uint256, SCPStatement> const& map,
    std::function<Hash(SCPStatement const&)> const& qfun,
    std::function<bool(uint256 const&, SCPStatement const&)> const& filter);

SCPQuorumSetPtr
Node::retrieveQuorumSet(Hash const& qSetHash)
{
    // Notify that we touched this node.
    mSCP->nodeTouched(mNodeID);

    SCPQuorumSetPtr ret = mSCP->getQSet(qSetHash);
    if(ret)
    {
        return ret;
    }

    CLOG(DEBUG, "SCP") << "Node::retrieveQuorumSet"
                       << "@" << hexAbbrev(mNodeID)
                       << " not found qSet: " << hexAbbrev(qSetHash);

    throw QuorumSetNotFound(mNodeID, qSetHash);
}


uint256 const&
Node::getNodeID()
{
    return mNodeID;
}

}
