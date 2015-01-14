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
    , mIsPristine(true)
    , mInAdvanceSlot(false)
    , mRunAdvanceSlot(false)
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

    // If the slot was already externalized, we emit a `COMMITTED (inf,x)` 
    // envelope.
    if (mBallot.counter == FBA_SLOT_MAX_COUNTER)
    {
        if (envelope.statement.ballot.counter != FBA_SLOT_MAX_COUNTER)
        {
            FBAEnvelope envelope = createEnvelope(FBAStatementType::COMMITTED);
            signEnvelope(envelope);

            mFBA->getClient()->emitEnvelope(envelope);
        }
        return;
    }

    if (envelope.statement.body.type() != FBAStatementType::PREPARE)
    {
        // isExternalized is true if we have a COMMITTED (inf,*) envelope. We
        // let these envelope go through as they don't bump the ballot counter
        // and are the only ones sent by a node that exteranlized.
        bool isExternalized = 
            (envelope.statement.ballot.counter == FBA_SLOT_MAX_COUNTER &&
             envelope.statement.body.type() == FBAStatementType::COMMITTED);

        // We don't bump ballots on other evenlopes than valid PREPARE ones.
        // So if the counter is different than the current one. We simply
        // ignore it unless isExternalized is true.
        if (!isExternalized && 
            envelope.statement.ballot.counter != mBallot.counter)
        {
            return;
        }

        // We accept envelopes other than PREPARE only if we previously saw a
        // valid envelope PREPARE for that ballot. This prevents node from
        // emitting phony messages too easily.
        if (isExternalized ||
            getNodeEnvelopes(envelope.nodeID, 
                             FBAStatementType::PREPARE).size() == 1)
        {
            FBABallot b = envelope.statement.ballot;
            FBAStatementType t = envelope.statement.body.type();

            // Store the envelope. Note that the ballot in this envelope can be
            // incompatible with mBallot (same counter, different valueHash)
            mEnvelopes[b.valueHash][t][envelope.nodeID] = envelope;
            advanceSlot();
        }
        else if (getNodeEnvelopes(envelope.nodeID, 
                                  FBAStatementType::PREPARE).size() == 0)
        {
            mFBA->getClient()->retransmissionHinted(mSlotIndex,
                                                    envelope.nodeID);
        }
    }
    else
    {
        Hash evidence = envelope.statement.body.prepare().evidence;
        FBABallot b = envelope.statement.ballot;
        FBAStatementType t = FBAStatementType::PREPARE;

        auto cb = [b,t,evidence,envelope,this] (bool valid)
        {
            // If the ballot is not valid, we just ignore it.
            if (!valid)
            {
                return;
            }

            // If a new higher ballot has been issued, let's move on to it.
            if (b.counter > mBallot.counter || isPristine())
            {
                bumpToBallot(b, evidence);
            }

            // If we already saw a PREPARE message from this node, we just
            // ignore this one as it is illegal.
            if (getNodeEnvelopes(envelope.nodeID, 
                                 FBAStatementType::PREPARE).size() != 0)
            {
              return;
            }

            // Finally store the envelope and advance the slot if possible.
            // Note that the ballot might be incompatible (same counter but
            // different valueHash)
            mEnvelopes[b.valueHash][t][envelope.nodeID] = envelope;
            advanceSlot();
        };

        mFBA->getClient()->validateBallot(mSlotIndex,
                                          envelope.nodeID,
                                          b,
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
    else if (isPristine())
    {
        mBallot.valueHash = valueHash;
        mEvidence = evidence;
    }
    else if (forceBump || mBallot.valueHash != valueHash)
    {
        bumpToBallot(FBABallot(mBallot.counter + 1, valueHash), evidence);
    }
    /* TODO(spolu): prevent in case of pledged to commit? */

    advanceSlot();
    return true;
}

void 
Slot::bumpToBallot(const FBABallot& ballot,
                   const Hash& evidence)
{
    LOG(INFO) << "Slot::bumpToBallot" 
              << "@" << binToHex(mFBA->getLocalNodeID()).substr(0,6)
              << " (" << ballot.counter
              << "," << binToHex(ballot.valueHash).substr(0,6) << ")"
              << " [" << binToHex(evidence).substr(0,6) << "]";
    if (!isPristine())
    {
        mFBA->getClient()->valueCancelled(mSlotIndex, 
                                          mBallot.valueHash);
    }
    mEnvelopes.clear();
    mBallot = ballot;
    mEvidence = evidence;
    mIsPristine = true;
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

    /*
    LOG(INFO) << "Slot::createEnvelope" 
              << "@" << binToHex(mFBA->getLocalNodeID()).substr(0,6)
              << " " << envToStr(envelope);
    */

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
        mEnvelopes[mBallot.valueHash][FBAStatementType::PREPARE]
            .find(mFBA->getLocalNodeID());
    if (it != mEnvelopes[mBallot.valueHash][FBAStatementType::PREPARE].end())
    {
        return;
    }
    LOG(INFO) << "Slot::attemptPrepare" 
              << "@" << binToHex(mFBA->getLocalNodeID()).substr(0,6)
              << " (" << mBallot.counter
              << ","  << binToHex(mBallot.valueHash).substr(0,6) << ")";

    FBAEnvelope envelope = createEnvelope(FBAStatementType::PREPARE);
    if (mPrepared.find(mBallot.valueHash) != mPrepared.end())
    {
        envelope.statement.body.prepare().prepared.activate() = 
          mPrepared[mBallot.valueHash];
    }
    for (auto b : mPledgedCommit)
    {
        envelope.statement.body.prepare().excepted.push_back(b);
    }
    envelope.statement.body.prepare().evidence = mEvidence;
    signEnvelope(envelope);

    mIsPristine = false;
    mFBA->getClient()->ballotDidPrepare(mSlotIndex, mBallot, mEvidence);
    mFBA->getClient()->emitEnvelope(envelope);

    processEnvelope(envelope);
}

void 
Slot::attemptPrepared()
{
    auto it = 
        mEnvelopes[mBallot.valueHash][FBAStatementType::PREPARED]
            .find(mFBA->getLocalNodeID());
    if (it != mEnvelopes[mBallot.valueHash][FBAStatementType::PREPARED].end())
    {
        return;
    }
    LOG(INFO) << "Slot::attemptPrepared" 
              << "@" << binToHex(mFBA->getLocalNodeID()).substr(0,6)
              << " (" << mBallot.counter
              << ","  << binToHex(mBallot.valueHash).substr(0,6) << ")";

    FBAEnvelope envelope = createEnvelope(FBAStatementType::PREPARED);
    signEnvelope(envelope);

    // Store the current ballot in mPrepared as the last ballot prepared for
    // the current value.
    mPrepared[mBallot.valueHash] = mBallot;

    mFBA->getClient()->emitEnvelope(envelope);

    processEnvelope(envelope);
}

void 
Slot::attemptCommit()
{
    auto it = 
        mEnvelopes[mBallot.valueHash][FBAStatementType::COMMIT]
            .find(mFBA->getLocalNodeID());
    if (it != mEnvelopes[mBallot.valueHash][FBAStatementType::COMMIT].end())
    {
        return;
    }
    LOG(INFO) << "Slot::attemptCommit" 
              << "@" << binToHex(mFBA->getLocalNodeID()).substr(0,6)
              << " (" << mBallot.counter
              << ","  << binToHex(mBallot.valueHash).substr(0,6) << ")";

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
        mEnvelopes[mBallot.valueHash][FBAStatementType::COMMITTED]
            .find(mFBA->getLocalNodeID());
    if (it != mEnvelopes[mBallot.valueHash][FBAStatementType::COMMITTED].end())
    {
        return;
    }
    LOG(INFO) << "Slot::attemptCommitted" 
              << "@" << binToHex(mFBA->getLocalNodeID()).substr(0,6)
              << " (" << mBallot.counter
              << ","  << binToHex(mBallot.valueHash).substr(0,6) << ")";

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
    LOG(INFO) << "Slot::attemptExternalize" 
              << "@" << binToHex(mFBA->getLocalNodeID()).substr(0,6)
              << " (" << mBallot.counter
              << ","  << binToHex(mBallot.valueHash).substr(0,6) << ")";

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
    LOG(DEBUG) << ">> Slot::nodeHasQuorum" 
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
    LOG(DEBUG) << ">> Slot::nodeIsVBlocking" 
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
Slot::isQuorumTransitive(const std::map<uint256, FBAEnvelope>& envelopes,
                         std::function<bool(const FBAEnvelope&)> const& filter)
{
    std::vector<uint256> pNodes;
    for (auto it : envelopes)
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
            FBAEnvelope e = envelopes.find(nodeID)->second;
            auto qSetHash = e.statement.quorumSetHash;
            return nodeHasQuorum(nodeID, qSetHash, pNodes);
        };
        auto it = std::copy_if(pNodes.begin(), pNodes.end(), 
                               fNodes.begin(), filter);
        fNodes.resize(std::distance(fNodes.begin(), it));
        pNodes = fNodes;
    } while (count != pNodes.size());

    return nodeHasQuorum(mFBA->getLocalNodeID(),
                         mFBA->getLocalNode()->getQuorumSetHash(),
                         pNodes);
    /*
    auto it = std::find(pNodes.begin(), pNodes.end(), nodeID);
    return (it != pNodes.end());
    */
}

bool 
Slot::isVBlocking(const std::map<uint256, FBAEnvelope>& envelopes,
                  const uint256& nodeID,
                  std::function<bool(const FBAEnvelope&)> const& filter)
{
    std::vector<uint256> pNodes;
    for (auto it : envelopes)
    {
        if (filter(it.second))
        {
            pNodes.push_back(it.first);
        }
    }

    Hash qSetHash;
    if (envelopes.find(nodeID) != envelopes.end())
    {
        FBAEnvelope e = envelopes.find(nodeID)->second;
        qSetHash = e.statement.quorumSetHash;
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
Slot::isPristine()
{
    return mIsPristine;
}

bool 
Slot::isPrepared(const Hash& valueHash)
{
    // Checks if we did not already accept the PREPARE statement.
    auto it = 
        mEnvelopes[valueHash][FBAStatementType::PREPARED]
            .find(mFBA->getLocalNodeID());
    if (it != mEnvelopes[valueHash][FBAStatementType::PREPARED].end())
    {
        return true;
    }

    // Checks if there is a v-blocking set of nodes that accepted the PREPARE
    // statements (this is an optimization).
    if (isVBlocking(mEnvelopes[valueHash][FBAStatementType::PREPARED],
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
            if (c.valueHash == mBallot.valueHash && 
                c.counter <= mBallot.counter)
            {
                continue;
            }
            
            /* TODO(spolu): 
             * Do we need to take every ballot into account?
             * Do we need to take PREPARED statement into account?
             */
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

            if (isVBlocking(mEnvelopes[valueHash][FBAStatementType::PREPARE],
                            envR.nodeID,
                            abortedFilter))
            {
                continue;
            }

            compOrAborted = false;
        }

        return compOrAborted;
    };

    if (isQuorumTransitive(mEnvelopes[valueHash][FBAStatementType::PREPARE],
                           ratifyFilter))
    {
        return true;
    }

    return false;
}

bool
Slot::isPreparedConfirmed(const Hash& valueHash)
{
    // Checks if there is a transitive quorum that accepted the PREPARE
    // statements for the local node.
    if (isQuorumTransitive(mEnvelopes[valueHash][FBAStatementType::PREPARED]))
    {
        return true;
    }
    return false;
}

bool 
Slot::isCommitted(const Hash& valueHash)
{
    // Checks if we did not already accept the COMMIT statement.
    auto it = 
        mEnvelopes[valueHash][FBAStatementType::COMMITTED]
            .find(mFBA->getLocalNodeID());
    if (it != mEnvelopes[valueHash][FBAStatementType::COMMITTED].end())
    {
        return true;
    }

    // Checks if there is a v-blocking set of nodes that accepted the COMMIT
    // statement (this is an optimization).
    if (isVBlocking(mEnvelopes[valueHash][FBAStatementType::COMMITTED],
                    mFBA->getLocalNodeID()))
    {
        return true;
    }

    // Check if we can establish the pledges for a transitive quorum.
    if (isQuorumTransitive(mEnvelopes[valueHash][FBAStatementType::COMMIT]))
    {
        return true;
    }

    return false;
}

bool 
Slot::isCommittedConfirmed(const Hash& valueHash)
{
    // Checks if there is a transitive quorum that accepted the COMMIT
    // statement for the local node.
    if (isQuorumTransitive(mEnvelopes[valueHash][FBAStatementType::COMMITTED]))
    {
        return true;
    }
    return false;
}

std::vector<FBAEnvelope> 
Slot::getNodeEnvelopes(const uint256& nodeID,
                       const FBAStatementType& type)
{
    std::vector<FBAEnvelope> envelopes;
    for (auto it : mEnvelopes)
    {
        if (it.second[type].find(nodeID) != it.second[type].end())
        {
            envelopes.push_back(it.second[type][nodeID]);
        }
    }
    return envelopes;
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
        // If we're pristine we pick the first ballot we find and advance the
        // protocol.
        if (isPristine()) 
        { 
            assert(mEnvelopes.size() <= 1);
            attemptPrepare(); 
        }

        for (auto it : mEnvelopes)
        {
            Hash valueHash = it.first;
            LOG(DEBUG) << "=> Slot::advanceSlot" 
                       << "@" << binToHex(mFBA->getLocalNodeID()).substr(0,6)
                       << " " << binToHex(valueHash).substr(0,6);

            // Given externalized node send COMMITTED (inf,x) messages we first
            // check if we can externalize directly
            if (isCommittedConfirmed(valueHash)) 
            { 
                if (valueHash != mBallot.valueHash)
                {
                    mBallot.valueHash = valueHash;
                }
                attemptExternalize(); 
            }

            if (isPrepared(valueHash)) 
            { 
                if (valueHash != mBallot.valueHash)
                {
                    mBallot.valueHash = valueHash;
                }
                attemptPrepared(); 
            }

            // If the ballot is confirmed as prepared we can move on to the
            // commit phase
            if (isPreparedConfirmed(valueHash))
            {
                assert(valueHash == mBallot.valueHash);
                attemptCommit();

                if (isCommitted(valueHash))
                {
                    attemptCommitted();
                }
            }
        }
    }
    catch(Node::QuorumSetNotFound e)
    {
        mFBA->getNode(e.nodeID())->addPendingSlot(e.qSetHash(), mSlotIndex);
        mFBA->getClient()->retrieveQuorumSet(e.nodeID(), e.qSetHash());
    }

    mInAdvanceSlot = false;
    if (mRunAdvanceSlot)
    {
        mRunAdvanceSlot = false;
        advanceSlot();
    }
}

}
