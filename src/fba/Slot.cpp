// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "Slot.h"

#include <cassert>
#include "util/types.h"
#include "util/Logging.h"
#include "fba/Node.h"
#include "fba/LocalNode.h"

namespace stellar
{

Slot::Slot(const uint32& slotIndex,
           FBA* FBA)
    : mSlotIndex(slotIndex)
    , mFBA(FBA)
{
    mBallot.counter = 0;
    assert(isZero(mBallot.valueHash));
}

void
Slot::processEnvelope(const FBAEnvelope& envelope)
{
    assert(envelope.statement.slotIndex == mSlotIndex);

    /*
    LOG(INFO) << "PROCESS [" << mFBA->getLocalNodeID() << "] " << mSlotIndex;
    LOG(INFO) << "{" << envelope.nodeID 
              << ":" << envelope.statement.body.type() 
              << "," << "(" << envelope.statement.ballot.counter 
              << " " << envelope.statement.ballot.valueHash << ")" << "}";
    */

    /* TODO(spolu): Check Signature */

    auto cb = [&] (bool valid)
    {
        if (valid)
        {
            mEnvelopes[envelope.statement.body.type()][envelope.nodeID] = 
              envelope;
            advanceSlot();
        }
    };

    mFBA->getClient()->validateBallot(mSlotIndex,
                                      envelope.nodeID,
                                      envelope.statement.ballot,
                                      cb);
}

bool
Slot::attemptValue(const uint256& valueHash)
{
    return false;
}

void 
Slot::attemptPrepare()
{
    auto it = 
        mEnvelopes[FBAStatementType::PREPARE].find(mFBA->getLocalNodeID());
    if (it != mEnvelopes[FBAStatementType::PREPARE].end())
    {
        return;
    }

    FBAEnvelope envelope;
    envelope.nodeID = mFBA->getLocalNodeID();
    envelope.statement.slotIndex = mSlotIndex;
    envelope.statement.ballot = mBallot;
    envelope.statement.quorumSetHash = mFBA->getLocalNode()->getQuorumSetHash();
    envelope.statement.body.type(FBAStatementType::PREPARE);
    /* TODO(spolu): Sign */

    mFBA->getClient()->emitEnvelope(envelope);
    processEnvelope(envelope);
}

void 
Slot::attemptPrepared()
{
    auto it = 
        mEnvelopes[FBAStatementType::PREPARED].find(mFBA->getLocalNodeID());
    if (it != mEnvelopes[FBAStatementType::PREPARED].end())
    {
        return;
    }
}

void 
Slot::attemptCommit()
{
    auto it = 
        mEnvelopes[FBAStatementType::COMMIT].find(mFBA->getLocalNodeID());
    if (it != mEnvelopes[FBAStatementType::COMMIT].end())
    {
        return;
    }
}

void 
Slot::attemptCommitted()
{
    auto it = 
        mEnvelopes[FBAStatementType::COMMITTED].find(mFBA->getLocalNodeID());
    if (it != mEnvelopes[FBAStatementType::COMMITTED].end())
    {
        return;
    }
}

void 
Slot::attemptExternalize()
{
    if (mBallot.counter == FBA_SLOT_MAX_COUNTER)
    {
        return;
    }
}

bool 
Slot::nodeHasQuorum(const uint256& nodeID,
                    const uint256& qSetHash,
                    const std::vector<uint256>& nodeSet)
{
    Node* node = mFBA->getNode(nodeID);
    // This call can throw a `QuorumSetNotFound` if the quorumSet is unknown.
    // The exception is catched in `advanceSlot`
    const FBAQuorumSet& qSet = node->retrieveQuorumSet(qSetHash);

    uint32 count = 0;
    for (auto n : qSet.validators)
    {
        auto it = std::find(nodeSet.begin(), nodeSet.end(), n);
        count += (it != nodeSet.end()) ? 1 : 0;
    }
    
    return (count >= qSet.threshold);
}

bool 
Slot::nodeIsVBlocking(const uint256& nodeID,
                      const uint256& qSetHash,
                      const std::vector<uint256>& nodeSet)
{
    Node* node = mFBA->getNode(nodeID);
    // This call can throw a `QuorumSetNotFound` if the quorumSet is unknown.
    // The exception is catched in `advanceSlot`
    const FBAQuorumSet& qSet = node->retrieveQuorumSet(qSetHash);

    uint32 count = 0;
    for (auto n : qSet.validators)
    {
        auto it = std::find(nodeSet.begin(), nodeSet.end(), n);
        count += (it != nodeSet.end()) ? 1 : 0;
    }
    
    return (qSet.validators.size() - count < qSet.threshold);
}

bool 
Slot::isQuorumTransitive(const FBAStatementType& type,
                         const uint256& nodeID,
                         std::function<bool(const FBAEnvelope&)> const& filter)
{
    std::vector<uint256> pNodes;
    for (auto it : mEnvelopes[type])
    {
        if (filter(it.second))
        {
            pNodes.push_back(it.first);
        }
    }

    size_t count = 0;
    do
    {
        count = pNodes.size();
        std::vector<uint256> fNodes(pNodes.size());
        auto filter = [&] (uint256 nodeID) -> bool 
        {
            auto qSetHash = mEnvelopes[type][nodeID].statement.quorumSetHash;
            return nodeHasQuorum(nodeID, qSetHash, pNodes);
        };
        auto it = std::copy_if(pNodes.begin(), pNodes.end(), 
                               fNodes.begin(), filter);
        fNodes.resize(std::distance(fNodes.begin(), it));
        pNodes = fNodes;
    } while (count != pNodes.size());

    auto it = std::find(pNodes.begin(), pNodes.end(), nodeID);
    return (it != pNodes.end());
}

bool 
Slot::isVBlocking(const FBAStatementType& type,
                  const uint256& nodeID,
                  std::function<bool(const FBAEnvelope&)> const& filter)
{
    std::vector<uint256> pNodes;
    for (auto it : mEnvelopes[type])
    {
        if (filter(it.second))
        {
            pNodes.push_back(it.first);
        }
    }

    auto qSetHash = mEnvelopes[type][nodeID].statement.quorumSetHash;
    return nodeIsVBlocking(nodeID, qSetHash, pNodes);
}

bool
Slot::isNull()
{
    auto it = 
        mEnvelopes[FBAStatementType::PREPARE].find(mFBA->getLocalNodeID());
    return (it == mEnvelopes[FBAStatementType::PREPARE].end());
}

bool 
Slot::isPrepared()
{
    // Checks if we did not already accept the PREPARE statement.
    auto it = 
        mEnvelopes[FBAStatementType::PREPARED].find(mFBA->getLocalNodeID());
    if (it != mEnvelopes[FBAStatementType::PREPARED].end())
    {
      return true;
    }

    // Checks if there is a v-blocking set of nodes that accepted the PREPARE
    // statements.
    if (isVBlocking(FBAStatementType::PREPARED,
                    mFBA->getLocalNodeID()))
    {
        return true;
    }

    // Check if we can establish the pledges for a transitive quorum.
    auto ratifyFilter = [&] (const FBAEnvelope& env) -> bool
    {
        // Either this node has no excepted B_c ballot
        if (env.statement.body.excepted().size() == 0)
        {
            return true;
        }

        // Or they are all compatible with a smaller counter
        bool compatible = true;
        for (auto b : env.statement.body.excepted())
        {
            if (b.valueHash != mBallot.valueHash ||
                b.counter > mBallot.counter)
            {
                compatible = false;
            }
        }
        if (compatible)
        {
            return true;
        }

        // Or it sees a v-blocking set of nodes that has prepared (as the
        // ballots in its excepted are all smaller than the current one)
        if (isVBlocking(FBAStatementType::PREPARED,
                        env.nodeID))
        {
            return true;
        }

        return false;
    };
    if(isQuorumTransitive(FBAStatementType::PREPARE,
                          mFBA->getLocalNodeID(),
                          ratifyFilter))
    {
        return true;
    }

    return false;
}

bool
Slot::isPreparedConfirmed()
{
    // Checks if there is a transitive quorum that accepted the PREPARE
    // statements for the local node.
    if(isQuorumTransitive(FBAStatementType::PREPARED,
                          mFBA->getLocalNodeID()))
    {
        return true;
    }
    return false;
}

bool 
Slot::isCommitted()
{
    // Checks if we did not already accept the COMMIT statement.
    auto it = 
        mEnvelopes[FBAStatementType::COMMITTED].find(mFBA->getLocalNodeID());
    if (it != mEnvelopes[FBAStatementType::COMMITTED].end())
    {
        return true;
    }

    // Checks if there is a v-blocking set of nodes that accepted the COMMIT
    // statement.
    if (isVBlocking(FBAStatementType::COMMITTED,
                    mFBA->getLocalNodeID()))
    {
        return true;
    }

    // Check if we can establish the pledges for a transitive quorum.
    if(isQuorumTransitive(FBAStatementType::COMMIT,
                          mFBA->getLocalNodeID()))
    {
        return true;
    }

    return false;
}

bool 
Slot::isCommittedConfirmed()
{
    // Checks if there is a transitive quorum that accepted the COMMIT
    // statement for the local node.
    if(isQuorumTransitive(FBAStatementType::COMMITTED,
                          mFBA->getLocalNodeID()))
    {
        return true;
    }
    return false;
}

void
Slot::advanceSlot()
{
    try
    {
        // PREPARE Phase
        if (isNull()) { attemptPrepare(); }
        if (isPrepared()) { attemptPrepared(); }

        // If the ballot is confirmed as prepared we can move on to the commit
        // phase
        if (!isPreparedConfirmed()) { return; }

        // COMMIT Phase
        attemptCommit();
        if (isCommitted()) { attemptCommitted(); }
        if (isCommittedConfirmed()) { attemptExternalize(); }
    }
    catch(Node::QuorumSetNotFound e)
    {
        mFBA->getClient()->retrieveQuorumSet(e.nodeID(), e.qSetHash());
        /* TODO(spolu): Store quorumSetHash awaiting */
    }
}

}
