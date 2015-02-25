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
    CLOG(DEBUG, "FBA") << "Node::hasQuorum" 
        << "@" << binToHex(mNodeID).substr(0,6)
        << " qSet: " << binToHex(qSetHash).substr(0,6)
        << " nodeSet.size: " << nodeSet.size();
    // This call can throw a `QuorumSetNotFound` if the quorumSet is unknown.
    const FBAQuorumSet& qSet = retrieveQuorumSet(qSetHash);

    uint32 count = 0;
    for (auto n : qSet.validators)
    {
        auto it = std::find(nodeSet.begin(), nodeSet.end(), n);
        count += (it != nodeSet.end()) ? 1 : 0;
    }
    auto result = (count >= qSet.threshold);
    CLOG(DEBUG, "FBA") << "Node::hasQuorum"
        << "@" << binToHex(mNodeID).substr(0, 6)
        << " is " << result;
        return result;
}

bool 
Node::isVBlocking(const Hash& qSetHash,
                  const std::vector<uint256>& nodeSet)
{
    CLOG(DEBUG, "FBA") << "Node::isVBlocking" 
        << "@" << binToHex(mNodeID).substr(0,6)
        << " qSet: " << binToHex(qSetHash).substr(0,6)
        << " nodeSet.size: " << nodeSet.size();
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
    auto result = (qSet.validators.size() - count < qSet.threshold);
    CLOG(DEBUG, "FBA") << "Node::isVBlocking"
        << "@" << binToHex(mNodeID).substr(0, 6)
        << " is " << result;
    return result;
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

template bool 
Node::isVBlocking<bool>(
    const Hash& qSetHash,
    const std::map<uint256, bool>& map,
    std::function<bool(const uint256&, const bool&)> const& filter);


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


const FBAQuorumSet& 
Node::retrieveQuorumSet(const uint256& qSetHash)
{
    // Notify that we touched this node.
    mFBA->nodeTouched(mNodeID);

    assert(mCacheLRU.size() == mCache.size());
    auto it = mCache.find(qSetHash);
    if (it != mCache.end()) 
    {
        return it->second;
    }

    CLOG(DEBUG, "FBA") << "Node::retrieveQuorumSet"
        << "@" << binToHex(mNodeID).substr(0,6)
        << " qSet: "  << binToHex(qSetHash).substr(0,6);

    throw QuorumSetNotFound(mNodeID, qSetHash);
}

void
Node::cacheQuorumSet(const FBAQuorumSet& qSet)
{
    uint256 qSetHash = sha256(xdr::xdr_to_msg(qSet));
    CLOG(DEBUG, "FBA") << "Node::cacheQuorumSet"
        << "@" << binToHex(mNodeID).substr(0,6)
        << " qSet: "  << binToHex(qSetHash).substr(0,6);

    if (mCache.find(qSetHash) != mCache.end())
    {
        return;
    }

    while (mCacheCapacity >= 0 && 
           mCache.size() >= (size_t)mCacheCapacity) 
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
