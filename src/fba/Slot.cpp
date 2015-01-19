// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "Slot.h"

#include <cassert>
#include "util/types.h"
#include "xdrpp/marshal.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "util/Logging.h"
#include "fba/Node.h"
#include "fba/LocalNode.h"

namespace stellar
{

// Static helper to stringify bellot for logging
static std::string
ballotToStr(const FBABallot& ballot)
{
    std::ostringstream oss;

    uint256 valueHash = 
      sha512_256(xdr::xdr_to_msg(ballot.value));

    oss << "(" << ballot.counter
        << "," << binToHex(valueHash).substr(0,6) << ")";
    return oss.str();
}

// Static helper to stringify envelope for logging
static std::string
envToStr(const FBAEnvelope& envelope)
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

    oss << "|" << ballotToStr(envelope.statement.ballot)
        << "|" << binToHex(envelope.statement.quorumSetHash).substr(0,6) << "}";
    return oss.str();
}

// Static helper to sign an envelope
static void
signEnv(FBAEnvelope& envelope)
{
    // TODO(spolu)
}


// Static helper to verify an envelope signature
static bool 
verifyEnv(const FBAEnvelope& envelope)
{
    // TODO(spolu)
    return true;
}


Slot::Slot(const uint64& slotIndex,
           FBA* FBA)
    : mSlotIndex(slotIndex)
    , mFBA(FBA)
    , mIsPristine(true)
    , mIsCommitted(false)
    , mIsExternalized(false)
    , mInAdvanceSlot(false)
    , mRunAdvanceSlot(false)
{
}

bool
Slot::processEnvelope(const FBAEnvelope& envelope)
{
    assert(envelope.statement.slotIndex == mSlotIndex);

    LOG(INFO) << "Slot::processEnvelope" 
              << "@" << binToHex(mFBA->getLocalNodeID()).substr(0,6)
              << ":" << mSlotIndex
              << " " << envToStr(envelope);

    // If the envelope is not correctly signed, we ignore it.
    if (!verifyEnv(envelope))
    {
        return false;
    }

    FBABallot b = envelope.statement.ballot;
    FBAStatementType t = envelope.statement.body.type();
    uint256 nodeID = envelope.nodeID;
    FBAStatement statement = envelope.statement;

    // We copy everything we need as this can be async (no reference).
    auto cb = [b,t,nodeID,statement,this] (bool valid)
    {
        // If the ballot is not valid, we just ignore it.
        if (!valid)
        {
            return;
        }

        if (!mIsCommitted && t == FBAStatementType::PREPARE)
        {
            // If a new higher ballot has been issued, let's move on to it.
            if (b.counter > mBallot.counter || isPristine())
            {
                bumpToBallot(b);
            }

            // Finally store the statement and advance the slot if possible.
            mStatements[b][t][nodeID] = statement;
            advanceSlot();
        }
        else if (!mIsCommitted && t == FBAStatementType::PREPARED)
        {
            // We accept PREPARED statements only if we previously saw a valid
            // PREPARE statement for that round. This prevents node from
            // emitting phony messages too easily.
            bool isPrepare = false;
            for (auto s : getNodeStatements(nodeID, FBAStatementType::PREPARE))
            {
                if (s.ballot.counter == b.counter)
                {
                    isPrepare = true;
                }
            }
            if (isPrepare)
            {
                // Finally store the statement and advance the slot if
                // possible.
                mStatements[b][t][nodeID] = statement;
                advanceSlot();
            }
            else
            {
                mFBA->getClient()->retransmissionHinted(mSlotIndex, nodeID);
            }
        }
        else if (!mIsCommitted && t == FBAStatementType::COMMIT)
        {
            // We accept COMMIT statements only if we previously saw a valid
            // PREPARE statement for that ballot and all the PREPARE we saw so
            // far have a lower ballot than this one or have that COMMIT in
            // their B_c.  This prevents node from emitting phony messages too
            // easily.
            bool isPrepare = false;
            for (auto s : getNodeStatements(nodeID, FBAStatementType::PREPARE))
            {
                if (compareBallots(b, s.ballot) == 0)
                {
                    isPrepare = true;
                }
                if (compareBallots(b, s.ballot) < 0)
                {
                    auto excepted = s.body.prepare().excepted;
                    auto it = std::find(excepted.begin(), excepted.end(), b);
                    if (it == excepted.end())
                    {
                        return;
                    }
                }
            }
            if (isPrepare)
            {
                // Finally store the statement and advance the slot if
                // possible.
                mStatements[b][t][nodeID] = statement;
                advanceSlot();
            }
            else
            {
                mFBA->getClient()->retransmissionHinted(mSlotIndex, nodeID);
            }
            
        }
        else if (t == FBAStatementType::COMMITTED)
        {
            // If we already have a COMMITTED statements for this node, we just
            // ignore this one as it is illegal.
            if (getNodeStatements(nodeID, 
                                  FBAStatementType::COMMITTED).size() > 0)
            {
                return;
            }

            // Finally store the statement and advance the slot if possible.
            mStatements[b][t][nodeID] = statement;
            advanceSlot();
        }
        else if(mIsCommitted)
        {
            // If the slot already COMMITTED and we received another statement,
            // we resend our own COMMITTED message.
            FBAStatement statement = 
                createStatement(FBAStatementType::COMMITTED);

            FBAEnvelope envelope = createEnvelope(statement);
            mFBA->getClient()->emitEnvelope(envelope);
        }
    };

    mFBA->getClient()->validateBallot(mSlotIndex, nodeID, b, cb);
    return true;
}

bool
Slot::attemptValue(const Value& value,
                   bool forceBump)
{
    if (mIsCommitted)
    {
        return false;
    }
    else if (isPristine() ||
             mFBA->getClient()->compareValues(mBallot.value, value) < 0)
    {
        bumpToBallot(FBABallot(mBallot.counter, value));
    }
    else if (forceBump || 
             mFBA->getClient()->compareValues(mBallot.value, value) > 0)
    {
        bumpToBallot(FBABallot(mBallot.counter + 1, value));
    }

    advanceSlot();
    return true;
}

void 
Slot::bumpToBallot(const FBABallot& ballot)
{
    LOG(INFO) << "Slot::bumpToBallot" 
              << "@" << binToHex(mFBA->getLocalNodeID()).substr(0,6)
              << " " << ballotToStr(ballot);

    if (!isPristine())
    {
        mFBA->getClient()->valueCancelled(mSlotIndex, 
                                          mBallot.value);
    }

    // We shouldnt have emitted any prepare message for this ballot or any
    // other higher ballot.
    for (auto s : getNodeStatements(mFBA->getLocalNodeID(), 
                                    FBAStatementType::PREPARE))
    {
        assert(compareBallots(ballot, s.ballot) >= 0);
    }
    // We should move mBallot monotically only
    assert(compareBallots(ballot, mBallot) > 0);

    mBallot = ballot;

    mIsPristine = true;
}


FBAStatement
Slot::createStatement(const FBAStatementType& type)
{
    FBAStatement statement;

    statement.slotIndex = mSlotIndex;
    statement.ballot = mBallot;
    statement.quorumSetHash = mFBA->getLocalNode()->getQuorumSetHash();
    statement.body.type(type);

    return statement;
}

FBAEnvelope
Slot::createEnvelope(const FBAStatement& statement)
{
    FBAEnvelope envelope;

    envelope.nodeID = mFBA->getLocalNodeID();
    envelope.statement = statement;
    signEnv(envelope);

    /*
    LOG(INFO) << "Slot::createEnvelope" 
              << "@" << binToHex(mFBA->getLocalNodeID()).substr(0,6)
              << " " << envToStr(envelope);
    */

    return envelope;
}

void 
Slot::attemptPrepare()
{
    auto it = 
        mStatements[mBallot][FBAStatementType::PREPARE]
            .find(mFBA->getLocalNodeID());
    if (it != mStatements[mBallot][FBAStatementType::PREPARE].end())
    {
        return;
    }
    LOG(INFO) << "Slot::attemptPrepare" 
              << "@" << binToHex(mFBA->getLocalNodeID()).substr(0,6)
              << " " << ballotToStr(mBallot);

    FBAStatement statement = createStatement(FBAStatementType::PREPARE);

    for (auto s : getNodeStatements(mFBA->getLocalNodeID(), 
                                    FBAStatementType::PREPARED))
    {
        if(s.ballot.value == mBallot.value)
        {
            if(!statement.body.prepare().prepared ||
               compareBallots(*(statement.body.prepare().prepared),
                              s.ballot) > 0)
            {
                statement.body.prepare().prepared.activate() = s.ballot;
            }
        }
    }
    for (auto s : getNodeStatements(mFBA->getLocalNodeID(), 
                                    FBAStatementType::COMMIT))
    {
        statement.body.prepare().excepted.push_back(s.ballot);
    }

    mIsPristine = false;
    mFBA->getClient()->ballotDidPrepare(mSlotIndex, mBallot);

    FBAEnvelope envelope = createEnvelope(statement);
    mFBA->getClient()->emitEnvelope(envelope);
    processEnvelope(envelope);
}

void 
Slot::attemptPrepared()
{
    auto it = 
        mStatements[mBallot][FBAStatementType::PREPARED]
            .find(mFBA->getLocalNodeID());
    if (it != mStatements[mBallot][FBAStatementType::PREPARED].end())
    {
        return;
    }
    LOG(INFO) << "Slot::attemptPrepared" 
              << "@" << binToHex(mFBA->getLocalNodeID()).substr(0,6)
              << " " << ballotToStr(mBallot);

    FBAStatement statement = createStatement(FBAStatementType::PREPARED);

    mIsPristine = false;

    FBAEnvelope envelope = createEnvelope(statement);
    mFBA->getClient()->emitEnvelope(envelope);
    processEnvelope(envelope);
}

void 
Slot::attemptCommit()
{
    auto it = 
        mStatements[mBallot][FBAStatementType::COMMIT]
            .find(mFBA->getLocalNodeID());
    if (it != mStatements[mBallot][FBAStatementType::COMMIT].end())
    {
        return;
    }
    LOG(INFO) << "Slot::attemptCommit" 
              << "@" << binToHex(mFBA->getLocalNodeID()).substr(0,6)
              << " " << ballotToStr(mBallot);

    FBAStatement statement = createStatement(FBAStatementType::COMMIT);

    mIsPristine = false;
    mFBA->getClient()->ballotDidCommit(mSlotIndex, mBallot);

    FBAEnvelope envelope = createEnvelope(statement);
    mFBA->getClient()->emitEnvelope(envelope);
    processEnvelope(envelope);
}

void 
Slot::attemptCommitted()
{
    auto it = 
        mStatements[mBallot][FBAStatementType::COMMITTED]
            .find(mFBA->getLocalNodeID());
    if (it != mStatements[mBallot][FBAStatementType::COMMITTED].end())
    {
        return;
    }
    LOG(INFO) << "Slot::attemptCommitted" 
              << "@" << binToHex(mFBA->getLocalNodeID()).substr(0,6)
              << " " << ballotToStr(mBallot);

    FBAStatement statement = createStatement(FBAStatementType::COMMITTED);

    mIsCommitted = true;
    mIsPristine = false;

    FBAEnvelope envelope = createEnvelope(statement);
    mFBA->getClient()->emitEnvelope(envelope);
    processEnvelope(envelope);
}

void 
Slot::attemptExternalize()
{
    if(mIsExternalized)
    {
        return;
    }
    LOG(INFO) << "Slot::attemptExternalize" 
              << "@" << binToHex(mFBA->getLocalNodeID()).substr(0,6)
              << " " << ballotToStr(mBallot);

    mIsExternalized = true;
    mIsPristine = false;

    mFBA->getClient()->valueExternalized(mSlotIndex, mBallot.value);
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

bool 
Slot::isQuorumTransitive(const std::map<uint256, FBAStatement>& statements,
                         std::function<bool(const uint256&, 
                                            const FBAStatement&)> const& filter)
{
    std::vector<uint256> pNodes;
    for (auto it : statements)
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
        auto filter = [&] (uint256 nodeID) -> bool 
        {
            FBAStatement s = statements.find(nodeID)->second;
            auto qSetHash = s.quorumSetHash;
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
}

bool 
Slot::isVBlocking(const std::map<uint256, FBAStatement>& statements,
                  const uint256& nodeID,
                  std::function<bool(const uint256&, 
                                     const FBAStatement&)> const& filter)
{
    std::vector<uint256> pNodes;
    for (auto it : statements)
    {
        if (filter(it.first, it.second))
        {
            pNodes.push_back(it.first);
        }
    }

    Hash qSetHash;
    if (statements.find(nodeID) != statements.end())
    {
        FBAStatement s = statements.find(nodeID)->second;
        qSetHash = s.quorumSetHash;
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
Slot::isPrepared(const FBABallot& ballot)
{
    // Checks if we haven't already emitted PREPARED b
    auto it = 
        mStatements[ballot][FBAStatementType::PREPARED]
            .find(mFBA->getLocalNodeID());
    if (it != mStatements[ballot][FBAStatementType::PREPARED].end())
    {
        return true;
    }

    // Checks if there is a v-blocking set of nodes that accepted the PREPARE
    // statements (this is an optimization).
    if (isVBlocking(mStatements[ballot][FBAStatementType::PREPARED],
                    mFBA->getLocalNodeID()))
    {
        return true;
    }

    // Check if we can establish the pledges for a transitive quorum.
    auto ratifyFilter = [&] (const uint256& nIDR, 
                             const FBAStatement& stR) -> bool
    {
        // Either the ratifying node has no excepted B_c ballot
        if (stR.body.prepare().excepted.size() == 0)
        {
            return true;
        }

        // The ratifying node have all its excepted B_c ballots compatible or
        // aborted. They are aborted if there is a v-blocking set of nodes that
        // prepared a higher ballot
        bool compOrAborted = true;
        for (auto c : stR.body.prepare().excepted)
        {
            if (c.value == mBallot.value)
            {
                continue;
            }
            
            auto abortedFilter = [&] (const uint256& nIDA,
                                      const FBAStatement& stA) -> bool
            {
                if (stA.body.prepare().prepared) 
                {
                    FBABallot p = *(stA.body.prepare().prepared);
                    if (compareBallots(p, c) > 0)
                    {
                        return true;
                    }
                }
                return false;
            };

            if (isVBlocking(mStatements[ballot][FBAStatementType::PREPARE],
                            nIDR,
                            abortedFilter))
            {
                continue;
            }

            compOrAborted = false;
        }

        return compOrAborted;
    };

    if (isQuorumTransitive(mStatements[ballot][FBAStatementType::PREPARE],
                           ratifyFilter))
    {
        return true;
    }

    return false;
}

bool
Slot::isPreparedConfirmed(const FBABallot& ballot)
{
    // Checks if we haven't already emitted COMMIT b
    auto it = 
        mStatements[ballot][FBAStatementType::COMMIT]
            .find(mFBA->getLocalNodeID());
    if (it != mStatements[ballot][FBAStatementType::COMMIT].end())
    {
        return true;
    }

    // Checks if there is a transitive quorum that accepted the PREPARE
    // statements for the local node.
    if (isQuorumTransitive(mStatements[ballot][FBAStatementType::PREPARED]))
    {
        return true;
    }
    return false;
}

bool 
Slot::isCommitted(const FBABallot& ballot)
{
    // Checks if we haven't already emitted COMMITTED b
    auto it = 
        mStatements[ballot][FBAStatementType::COMMITTED]
            .find(mFBA->getLocalNodeID());
    if (it != mStatements[ballot][FBAStatementType::COMMITTED].end())
    {
        return true;
    }

    // Check if we can establish the pledges for a transitive quorum.
    if (isQuorumTransitive(mStatements[ballot][FBAStatementType::COMMIT]))
    {
        return true;
    }

    return false;
}

bool 
Slot::isCommittedConfirmed(const Value& value)
{
    // TODO Extract ballots for value
    std::map<uint256, FBAStatement> statements;
    for (auto it : mStatements)
    {
        if (it.first.value == value)
        {
            for(auto sit : it.second[FBAStatementType::COMMITTED])
            {
                statements[sit.first] = sit.second;
            }
        }
    }

    // Checks if there is a transitive quorum that accepted the COMMIT
    // statement for the local node.
    if (isQuorumTransitive(statements))
    {
        return true;
    }
    return false;
}

std::vector<FBAStatement> 
Slot::getNodeStatements(const uint256& nodeID,
                        const FBAStatementType& type)
{
    std::vector<FBAStatement> statements;
    for (auto it : mStatements)
    {
        if (it.second[type].find(nodeID) != it.second[type].end())
        {
            statements.push_back(it.second[type][nodeID]);
        }
    }
    return statements;
}

int 
Slot::compareBallots(const FBABallot& b1, 
                     const FBABallot& b2)
{
    if (b1.counter < b2.counter)
    {
        return -1;
    }
    if (b2.counter < b1.counter)
    {
        return 1;
    }
    return mFBA->getClient()->compareValues(b1.value, b2.value);
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
        LOG(DEBUG) << "=> Slot::advanceSlot" 
                   << "@" << binToHex(mFBA->getLocalNodeID()).substr(0,6)
                   << " " << ballotToStr(mBallot);

        // If we're pristine we pick the first ballot we find and advance the
        // protocol.
        if (isPristine()) 
        { 
            attemptPrepare(); 
        }

        if (isPrepared(mBallot))
        {
            attemptPrepared(); 
        }

        // If our current ballot is prepared confirmed we can move onto the
        // commit phase
        if (isPreparedConfirmed(mBallot))
        {
            attemptCommit();

            if (isCommitted(mBallot))
            {
                attemptCommitted();
            }
        }

        // If our current ballot is committed and we can confirm the value then
        // we externalize
        if (mIsCommitted && isCommittedConfirmed(mBallot.value)) 
        {
            attemptExternalize(); 
        }

        // We loop on all known ballots to check if there are conditions that
        // should make us bump our current ballot
        for (auto it : mStatements)
        {
            // None of this apply if we committed or externalized
            if(mIsCommitted || mIsExternalized)
            {
                break;
            }

            FBABallot b = it.first;

            LOG(DEBUG) << "=> Slot::advanceSlot::tryBumping" 
                       << "@" << binToHex(mFBA->getLocalNodeID()).substr(0,6)
                       << " " << ballotToStr(mBallot);

            // If we could externalize by moving on to a given value we bump
            // our ballot to the apporpriate one
            if (isCommittedConfirmed(b.value)) 
            { 
                assert(!mIsCommitted || mBallot.value == b.value);

                // We look for the smallest ballot that is bigger than all the
                // COMMITTED message we saw for the value and our own current
                // ballot.
                FBABallot bext = FBABallot(mBallot.counter, b.value);
                if(compareBallots(bext, mBallot) < 0)
                {
                    bext.counter += 1;
                }
                for (auto it : mStatements)
                {
                    if (it.first.value == bext.value)
                    {
                        for(auto sit : it.second[FBAStatementType::COMMITTED])
                        {
                            if(compareBallots(bext, it.first) < 0)
                            {
                                bext = it.first;
                            }
                        }
                    }
                }

                bumpToBallot(bext);
                attemptCommitted();
            }

            // If a higher ballot has prepared, we can bump to it as our
            // current ballot has become irrelevant (aborted)
            if (compareBallots(b, mBallot) > 0 && isPrepared(b))
            {
                bumpToBallot(b);
                mRunAdvanceSlot = true;
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
