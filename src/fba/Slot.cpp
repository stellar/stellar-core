// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "Slot.h"

#include <cassert>
#include "util/types.h"
#include "crypto/Hex.h"
#include "util/Logging.h"
#include "fba/Node.h"
#include "fba/LocalNode.h"

namespace stellar
{

Slot::Slot(const uint32& slotIndex,
           FBA* FBA)
    : mSlotIndex(slotIndex)
    , mFBA(FBA)
    , mInAdvanceSlot(false)
{
    mBallot.counter = 0;
    assert(isZero(mBallot.valueHash));
}

void
Slot::processEnvelope(const FBAEnvelope& envelope)
{
    assert(envelope.statement.slotIndex == mSlotIndex);

    LOG(INFO) << "Slot::processEnvelope" 
              << "@" << binToHex(mFBA->getLocalNodeID()).substr(0,6)
              << ":" << mSlotIndex
              << " " << envToStr(envelope);

    // If the envelope is not correctly signed, we ignore it.
    if (!verifyEnvelope(envelope))
    {
        return;
    }

    // If the slot was already externalized, we ignore the envelope.
    if (mBallot.counter == FBA_SLOT_MAX_COUNTER)
    {
        return;
    }

    // We only accept envelopes that are not PREPARE if we already accepted a
    // valid envelope with PREPARE type.
    if (envelope.statement.body.type() != FBAStatementType::PREPARE)
    {
        // If the ballot is incompatible or with a higher counter, we ignore
        // it. We refuse to bump ballots on other messages than valid PREPARE
        // ones.
        if (envelope.statement.ballot.valueHash != mBallot.valueHash ||
            envelope.statement.ballot.counter != mBallot.counter)
        {
            return;
        }

        if (mEnvelopes[FBAStatementType::PREPARE].find(envelope.nodeID) !=
            mEnvelopes[FBAStatementType::PREPARE].end())
        {
            mEnvelopes[envelope.statement.body.type()][envelope.nodeID] = 
                envelope;
            // We also update the PREARE envelope with the newly prepared
            // ballot value
            FBAEnvelope& prepareEnvelope =
              mEnvelopes[FBAStatementType::PREPARE][envelope.nodeID];
            prepareEnvelope.statement.body.prepare().prepared.activate() =
               mBallot;

            advanceSlot();
        }
    }
    else
    {
        Hash evidence = envelope.statement.body.prepare().evidence;
        FBABallot ballot = envelope.statement.ballot;

        auto cb = [ballot,evidence,envelope,this] (bool valid)
        {
            // If the ballot is not valid, we just ignore it.
            if (!valid)
            {
                return;
            }

            // If a new higher ballot has been issued, let's move on to it.
            if (ballot.counter > mBallot.counter)
            {
                if (!isNull())
                {
                    mFBA->getClient()->valueCancelled(mSlotIndex, 
                                                      mBallot.valueHash);
                }
                mEnvelopes.empty();
                mBallot = ballot;
                mEvidence = evidence;
            }

            // If the ballot is incompatible, we ignore it.
            if (ballot.valueHash != mBallot.valueHash)
            {
                return;
            }

            // Finally store the envelope and advance the slot if possible.
            mEnvelopes[FBAStatementType::PREPARE][envelope.nodeID] = envelope;

            advanceSlot();
        };

        mFBA->getClient()->validateBallot(mSlotIndex,
                                          envelope.nodeID,
                                          ballot,
                                          evidence,
                                          cb);
    }
}

bool
Slot::attemptValue(const Hash& valueHash,
                   const Hash& evidence,
                   bool forceBump)
{
    if (mBallot.counter == FBA_SLOT_MAX_COUNTER)
    {
        return false;
    }

    else if (forceBump || (!isNull() && mBallot.valueHash != valueHash))
    {
        mFBA->getClient()->valueCancelled(mSlotIndex, mBallot.valueHash);
        mEnvelopes.empty();

        mBallot.counter++;
    }
    mBallot.valueHash = valueHash;
    mEvidence = evidence;

    advanceSlot();
    return true;
}

FBAEnvelope
Slot::createEnvelope(const FBAStatementType& type)
{
    FBAEnvelope envelope;

    envelope.nodeID = mFBA->getLocalNodeID();
    envelope.statement.slotIndex = mSlotIndex;
    envelope.statement.ballot = mBallot;
    envelope.statement.quorumSetHash = mFBA->getLocalNode()->getQuorumSetHash();
    envelope.statement.body.type(type);

    LOG(INFO) << "Slot::createEnvelope" 
              << "@" << binToHex(mFBA->getLocalNodeID()).substr(0,6)
              << " " << envToStr(envelope);

    return envelope;
}

std::string
Slot::envToStr(const FBAEnvelope& envelope)
{
    std::ostringstream oss;
    oss << "{ENV@" << binToHex(envelope.nodeID).substr(0,6) << "|";
    switch(envelope.statement.body.type())
    {
        case FBAStatementType::PREPARE:
            oss << "PREPARE";
            break;
        case FBAStatementType::PREPARED:
            oss << "PREPARED";
            break;
        case FBAStatementType::COMMIT:
            oss << "COMMIT";
            break;
        case FBAStatementType::COMMITTED:
            oss << "COMMITTED";
            break;
    }
    oss << "|" << "(" << envelope.statement.ballot.counter 
        << "," << binToHex(envelope.statement.ballot.valueHash).substr(0,6) << ")"
        << "|" << binToHex(envelope.statement.quorumSetHash).substr(0,6) << "}";
    return oss.str();
}

void
Slot::signEnvelope(FBAEnvelope& envelope)
{
    /* TODO(spolu) */
}


bool 
Slot::verifyEnvelope(const FBAEnvelope& envelope)
{
    /* TODO(spolu) */
    return true;
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

    FBAEnvelope envelope = createEnvelope(FBAStatementType::PREPARE);
    if (mPrepared.find(mBallot.valueHash) != mPrepared.end())
    {
        envelope.statement.body.prepare().prepared.activate() = mBallot;
    }
    for (auto b : mPledgedCommit)
    {
        envelope.statement.body.prepare().excepted.push_back(b);
    }
    envelope.statement.body.prepare().evidence = mEvidence;
    signEnvelope(envelope);

    mFBA->getClient()->ballotDidPrepare(mSlotIndex, mBallot, mEvidence);
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

    FBAEnvelope envelope = createEnvelope(FBAStatementType::PREPARED);
    signEnvelope(envelope);

    // Store the current ballot in mPrepared as the last ballot prepared for
    // the current value.
    mPrepared[mBallot.valueHash] =  mBallot;

    mFBA->getClient()->emitEnvelope(envelope);
    processEnvelope(envelope);
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

    FBAEnvelope envelope = createEnvelope(FBAStatementType::COMMIT);
    signEnvelope(envelope);

    // Store the current ballot in the list of ballot we pledged to commit.
    mPledgedCommit.push_back(mBallot);

    mFBA->getClient()->ballotDidCommit(mSlotIndex, mBallot);
    mFBA->getClient()->emitEnvelope(envelope);
    processEnvelope(envelope);
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

    FBAEnvelope envelope = createEnvelope(FBAStatementType::COMMITTED);
    signEnvelope(envelope);

    mFBA->getClient()->emitEnvelope(envelope);
    processEnvelope(envelope);
}

void 
Slot::attemptExternalize()
{
    if (mBallot.counter == FBA_SLOT_MAX_COUNTER)
    {
        return;
    }

    mBallot.counter = FBA_SLOT_MAX_COUNTER;

    mPrepared[mBallot.valueHash] = mBallot;
    mPledgedCommit.push_back(mBallot);

    mFBA->getClient()->valueExternalized(mSlotIndex, mBallot.valueHash);
}

bool 
Slot::nodeHasQuorum(const uint256& nodeID,
                    const Hash& qSetHash,
                    const std::vector<uint256>& nodeSet)
{
    LOG(INFO) << "Slot::nodeHashQuorum" 
              << "@" << binToHex(mFBA->getLocalNodeID()).substr(0,6)
              << " [" << binToHex(nodeID).substr(0,6) << "]"
              << " " << binToHex(qSetHash).substr(0,6)
              << " " << nodeSet.size();
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
                      const Hash& qSetHash,
                      const std::vector<uint256>& nodeSet)
{
    LOG(INFO) << "Slot::nodeIsVBlocking" 
              << "@" << binToHex(mFBA->getLocalNodeID()).substr(0,6)
              << " [" << binToHex(nodeID).substr(0,6) << "]"
              << " " << binToHex(qSetHash).substr(0,6)
              << " " << nodeSet.size();
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

    Hash qSetHash;
    if (mEnvelopes[type].find(nodeID) != mEnvelopes[type].end())
    {
        qSetHash = mEnvelopes[type][nodeID].statement.quorumSetHash;
    }
    else if (nodeID == mFBA->getLocalNodeID())
    {
        qSetHash = mFBA->getLocalNode()->getQuorumSetHash();
    }
    else 
    {
        assert(false);
    }

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
    // statements (this is an optimization).
    if (isVBlocking(FBAStatementType::PREPARED,
                    mFBA->getLocalNodeID()))
    {
        return true;
    }

    // Check if we can establish the pledges for a transitive quorum.
    auto ratifyFilter = [&] (const FBAEnvelope& envR) -> bool
    {
        // Either the ratifying node has no excepted B_c ballot
        if (envR.statement.body.prepare().excepted.size() == 0)
        {
            return true;
        }

        // Or they are all compatible or aborted. They are aborted if there is
        // a v-blocking set of nodes that prepared the ballot with a higher
        // counter than the the excepted ballot c.
        bool compOrAborted = true;
        for (auto c : envR.statement.body.prepare().excepted)
        {
            auto abortedFilter = [&] (const FBAEnvelope& envA) -> bool
            {
                if (envA.statement.body.prepare().prepared) {
                    FBABallot p = *(envA.statement.body.prepare().prepared);
                    if (p.counter > c.counter)
                    {
                        return true;
                    }
                }
                return false;
            };

            if ((c.valueHash == mBallot.valueHash &&
                 c.counter <= mBallot.counter) ||
                isVBlocking(FBAStatementType::PREPARE,
                            envR.nodeID,
                            abortedFilter))
            {
                compOrAborted = false;
            }
        }

        return compOrAborted;
    };

    if (isQuorumTransitive(FBAStatementType::PREPARE,
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
    if (isQuorumTransitive(FBAStatementType::PREPARED,
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
    // statement (this is an optimization).
    if (isVBlocking(FBAStatementType::COMMITTED,
                    mFBA->getLocalNodeID()))
    {
        return true;
    }

    // Check if we can establish the pledges for a transitive quorum.
    if (isQuorumTransitive(FBAStatementType::COMMIT,
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
    if (isQuorumTransitive(FBAStatementType::COMMITTED,
                           mFBA->getLocalNodeID()))
    {
        return true;
    }
    return false;
}

void
Slot::advanceSlot()
{
    // `advanceSlot` will prevent recursive call by setting and checking
    // `mInAdvanceSlot`. If a recursive call is made, `mRunAdvanceSlot` will be
    // set and `advanceSlot` will be called again after it is done executing.
    if(mInAdvanceSlot)
    {
        mRunAdvanceSlot = true;
        return;
    }
    mInAdvanceSlot = true;

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
        mFBA->getNode(e.nodeID())->addPendingSlot(e.qSetHash(), mSlotIndex);
    }

    mInAdvanceSlot = false;
    if (mRunAdvanceSlot)
    {
        mRunAdvanceSlot = false;
        advanceSlot();
    }
}

}
