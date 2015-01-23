// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "Node.h"

#include <cassert>
#include "xdrpp/marshal.h"
#include "util/Logging.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"


namespace stellar
{

Node::Node(const uint256& nodeID,
           FBA* FBA,
           int cacheCapacity)
    : mNodeID(nodeID)
    , mFBA(FBA)
    , mCacheCapacity(cacheCapacity)
{
}

bool
Node::hasQuorum(const Hash& qSetHash,
                    const std::vector<uint256>& nodeSet)
{
    LOG(DEBUG) << ">> Node::hasQuorum" 
               << "@" << binToHex(mNodeID).substr(0,6)
               << " " << binToHex(qSetHash).substr(0,6)
               << " " << nodeSet.size();
    // This call can throw a `QuorumSetNotFound` if the quorumSet is unknown.
    const FBAQuorumSet& qSet = retrieveQuorumSet(qSetHash);

    uint32 count = 0;
    for (auto n : qSet.validators)
    {
        auto it = std::find(nodeSet.begin(), nodeSet.end(), n);
        count += (it != nodeSet.end()) ? 1 : 0;
    }
    return (count >= qSet.threshold);
}

bool 
Node::isVBlocking(const Hash& qSetHash,
                  const std::vector<uint256>& nodeSet)
{
    LOG(DEBUG) << ">> Node::isVBlocking" 
               << "@" << binToHex(mNodeID).substr(0,6)
               << " " << binToHex(qSetHash).substr(0,6)
               << " " << nodeSet.size();
    // This call can throw a `QuorumSetNotFound` if the quorumSet is unknown.
    const FBAQuorumSet& qSet = retrieveQuorumSet(qSetHash);

    // There is no v-blocking set for {\empty}
    if(qSet.threshold == 0)
    {
        return false;
    }

    uint32 count = 0;
    for (auto n : qSet.validators)
    {
        auto it = std::find(nodeSet.begin(), nodeSet.end(), n);
        count += (it != nodeSet.end()) ? 1 : 0;
    }
    return (qSet.validators.size() - count < qSet.threshold);
}

template <class T> bool 
Node::isVBlocking(const Hash& qSetHash,
                  const std::map<uint256, T>& map,
                  std::function<bool(const uint256&, const T&)> const& filter)
{
    std::vector<uint256> pNodes;
    for (auto it : map)
    {
        if (filter(it.first, it.second))
        {
            pNodes.push_back(it.first);
        }
    }

    return isVBlocking(qSetHash, pNodes);
}

template bool 
Node::isVBlocking<FBAStatement>(
    const Hash& qSetHash,
    const std::map<uint256, FBAStatement>& map,
    std::function<bool(const uint256&, const FBAStatement&)> const& filter);


template <class T> bool 
Node::isQuorumTransitive(const Hash& qSetHash,
                         const std::map<uint256, T>& map,
                         std::function<Hash(const T&)> const& qfun,
                         std::function<bool(const uint256&, const T&)> const&
                         filter)
{
    std::vector<uint256> pNodes;
    for (auto it : map)
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
        auto quorumFilter = [&] (uint256 nodeID) -> bool 
        {
            return mFBA->getNode(nodeID)->hasQuorum(
                qfun(map.find(nodeID)->second),
                pNodes);
        };
        auto it = std::copy_if(pNodes.begin(), pNodes.end(), 
                               fNodes.begin(), quorumFilter);
        fNodes.resize(std::distance(fNodes.begin(), it));
        pNodes = fNodes;
    } while (count != pNodes.size());

    return hasQuorum(qSetHash, pNodes);
}

template bool 
Node::isQuorumTransitive<FBAStatement>(
    const Hash& qSetHash,
    const std::map<uint256, FBAStatement>& map,
    std::function<Hash(const FBAStatement&)> const& qfun,
    std::function<bool(const uint256&, const FBAStatement&)> const& filter);

template bool 
Node::isQuorumTransitive<FBAEnvelope>(
    const Hash& qSetHash,
    const std::map<uint256, FBAEnvelope>& map,
    std::function<Hash(const FBAEnvelope&)> const& qfun,
    std::function<bool(const uint256&, const FBAEnvelope&)> const& filter);


const FBAQuorumSet& 
Node::retrieveQuorumSet(const uint256& qSetHash)
{
    /*
    LOG(DEBUG) << "Node::retrieveQuorumSet"
               << "@" << binToHex(mNodeID).substr(0,6)
               << " "  << binToHex(qSetHash).substr(0,6);
    */

    assert(mCacheLRU.size() == mCache.size());
    auto it = mCache.find(qSetHash);
    if (it != mCache.end()) 
    {
        return it->second;
    }
    throw QuorumSetNotFound(mNodeID, qSetHash);
}

void
Node::cacheQuorumSet(const FBAQuorumSet& qSet)
{
    uint256 qSetHash = sha512_256(xdr::xdr_to_msg(qSet));
    /*
    LOG(DEBUG) << "Node::cacheQuorumSet"
               << "@" << binToHex(mNodeID).substr(0,6)
               << " "  << binToHex(qSetHash).substr(0,6);
    */

    if (mCache.find(qSetHash) != mCache.end())
    {
        return;
    }

    while (mCacheCapacity >= 0 && 
           mCache.size() >= mCacheCapacity) 
    {
        assert(mCacheLRU.size() == mCache.size());
        auto it = mCacheLRU.begin();
        mCache.erase(*it);
        mCacheLRU.erase(it);
    }
    mCacheLRU.push_back(qSetHash);
    mCache[qSetHash] = qSet;
}

const uint256&
Node::getNodeID()
{
    return mNodeID;
}

}
