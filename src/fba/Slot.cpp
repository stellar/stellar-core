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
using xdr::operator==;
using xdr::operator<;

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
    switch(envelope.statement.pledges.type())
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

    Hash qSetHash = envelope.statement.quorumSetHash;
    oss << "|" << ballotToStr(envelope.statement.ballot);
    oss << "|" << binToHex(qSetHash).substr(0,6) << "}";
    return oss.str();
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

void
Slot::processEnvelope(const FBAEnvelope& envelope,
                      std::function<void(FBA::EnvelopeState)> const& cb)
{
    assert(envelope.slotIndex == mSlotIndex);

    LOG(INFO) << "Slot::processEnvelope" 
              << "@" << binToHex(mFBA->getLocalNodeID()).substr(0,6)
              << ":" << mSlotIndex
              << " " << envToStr(envelope);

    uint256 nodeID = envelope.nodeID;
    FBAStatement statement = envelope.statement;
    FBABallot b = statement.ballot;
    FBAStatementType t = statement.pledges.type();

    // We copy everything we need as this can be async (no reference).
    auto value_cb = [b,t,nodeID,statement,cb,this] (bool valid)
    {
        // If the value is not valid, we just ignore it.
        if (!valid)
        {
            return cb(FBA::EnvelopeState::INVALID);
        }

        if (!mIsCommitted && t == FBAStatementType::PREPARE)
        {
            auto ballot_cb = [b,t,nodeID,statement,cb,this] (bool valid)
            {
                // If the ballot is not valid, we just ignore it.
                if(!valid)
                {
                    return cb(FBA::EnvelopeState::INVALID);
                }

                // If a new higher ballot has been issued, let's move on to it.
                if (compareBallots(b, mBallot) > 0 || isPristine())
                {
                    bumpToBallot(b);
                }

                // Finally store the statement and advance the slot if possible.
                mStatements[b][t][nodeID] = statement;
                advanceSlot();
            };

            mFBA->validateBallot(mSlotIndex, nodeID, b,
                                 ballot_cb);
        }
        else if (!mIsCommitted && t == FBAStatementType::PREPARED)
        {
            // A PREPARED statement does not imply any PREPARE so we can go
            // ahead and store it if its value is valid.
            mStatements[b][t][nodeID] = statement;
            advanceSlot();
        }
        else if (!mIsCommitted && t == FBAStatementType::COMMIT)
        {
            // We accept COMMIT statements only if we previously saw a valid
            // PREPARED statement for that ballot and all the PREPARE we saw so
            // far have a lower ballot than this one or have that COMMIT in
            // their B_c.  This prevents node from emitting phony messages too
            // easily.
            bool isPrepared = false;
            for (auto s : getNodeStatements(nodeID, 
                                            FBAStatementType::PREPARED))
            {
                if (compareBallots(b, s.ballot) == 0)
                {
                    isPrepared = true;
                }
            }
            for (auto s : getNodeStatements(nodeID, FBAStatementType::PREPARE))
            {
                if (compareBallots(b, s.ballot) < 0)
                {
                    auto excepted = s.pledges.prepare().excepted;
                    auto it = std::find(excepted.begin(), excepted.end(), b);
                    if (it == excepted.end())
                    {
                        return cb(FBA::EnvelopeState::INVALID);
                    }
                }
            }
            if (isPrepared)
            {
                // Finally store the statement and advance the slot if
                // possible.
                mStatements[b][t][nodeID] = statement;
                advanceSlot();
            }
            else
            {
                return cb(FBA::EnvelopeState::STATEMENTS_MISSING);
            }
            
        }
        else if (t == FBAStatementType::COMMITTED)
        {
            // If we already have a COMMITTED statements for this node, we just
            // ignore this one as it is illegal.
            if (getNodeStatements(nodeID, 
                                  FBAStatementType::COMMITTED).size() > 0)
            {
                return cb(FBA::EnvelopeState::INVALID);
            }

            // Finally store the statement and advance the slot if possible.
            mStatements[b][t][nodeID] = statement;
            advanceSlot();
        }
        else if(mIsCommitted)
        {
            // If the slot already COMMITTED and we received another statement,
            // we resend our own COMMITTED message.
            FBAStatement stmt =
                createStatement(FBAStatementType::COMMITTED);

            FBAEnvelope env = createEnvelope(stmt);
            mFBA->emitEnvelope(env);
        }

        // Finally call the callback saying that this was a valid envelope
        return cb(FBA::EnvelopeState::VALID);
    };

    mFBA->validateValue(mSlotIndex, nodeID, b.value, value_cb);
}

bool
Slot::prepareValue(const Value& value,
                   bool forceBump)
{
    if (mIsCommitted)
    {
        return false;
    }
    else if (isPristine() || mFBA->compareValues(mBallot.value, value) < 0)
    {
        bumpToBallot(FBABallot(mBallot.counter, value));
    }
    else if (forceBump || mFBA->compareValues(mBallot.value, value) > 0)
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
        mFBA->ballotDidAbort(mSlotIndex, mBallot);
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

    statement.ballot = mBallot;
    statement.quorumSetHash = mFBA->getLocalNode()->getQuorumSetHash();
    statement.pledges.type(type);

    return statement;
}

FBAEnvelope
Slot::createEnvelope(const FBAStatement& statement)
{
    FBAEnvelope envelope;

    envelope.nodeID = mFBA->getLocalNodeID();
    envelope.slotIndex = mSlotIndex;
    envelope.statement = statement;
    mFBA->signEnvelope(envelope);

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
            if(!statement.pledges.prepare().prepared ||
               compareBallots(*(statement.pledges.prepare().prepared),
                              s.ballot) > 0)
            {
                statement.pledges.prepare().prepared.activate() = s.ballot;
            }
        }
    }
    for (auto s : getNodeStatements(mFBA->getLocalNodeID(), 
                                    FBAStatementType::COMMIT))
    {
        statement.pledges.prepare().excepted.push_back(s.ballot);
    }

    mIsPristine = false;
    mFBA->ballotDidPrepare(mSlotIndex, mBallot);

    FBAEnvelope envelope = createEnvelope(statement);
    auto cb = [envelope,this] (bool valid)
    {
        mFBA->emitEnvelope(envelope);
    };
    processEnvelope(envelope, cb);
                        
}

void 
Slot::attemptPrepared(const FBABallot& ballot)
{
    auto it = 
        mStatements[ballot][FBAStatementType::PREPARED]
            .find(mFBA->getLocalNodeID());
    if (it != mStatements[ballot][FBAStatementType::PREPARED].end())
    {
        return;
    }
    LOG(INFO) << "Slot::attemptPrepared" 
              << "@" << binToHex(mFBA->getLocalNodeID()).substr(0,6)
              << " " << ballotToStr(ballot);

    FBAStatement statement = createStatement(FBAStatementType::PREPARED);
    statement.ballot = ballot;

    if (ballot == mBallot)
    {
        mIsPristine = false;
    }

    FBAEnvelope envelope = createEnvelope(statement);
    auto cb = [envelope,this] (bool valid)
    {
        mFBA->emitEnvelope(envelope);
    };
    processEnvelope(envelope, cb);
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
    mFBA->ballotDidCommit(mSlotIndex, mBallot);

    FBAEnvelope envelope = createEnvelope(statement);
    auto cb = [envelope,this] (bool valid)
    {
        mFBA->emitEnvelope(envelope);
    };
    processEnvelope(envelope, cb);
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
    auto cb = [envelope,this] (bool valid)
    {
        mFBA->emitEnvelope(envelope);
    };
    processEnvelope(envelope, cb);
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

    mFBA->valueExternalized(mSlotIndex, mBallot.value);
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
    if (mFBA->getLocalNode()->isVBlocking<FBAStatement>(
            mFBA->getLocalNode()->getQuorumSetHash(),
            mStatements[ballot][FBAStatementType::PREPARED]))
    {
        return true;
    }

    // Check if we can establish the pledges for a transitive quorum.
    auto ratifyFilter = [&] (const uint256& nIDR, 
                             const FBAStatement& stR) -> bool
    {
        // Either the ratifying node has no excepted B_c ballot
        if (stR.pledges.prepare().excepted.size() == 0)
        {
            return true;
        }

        // The ratifying node have all its excepted B_c ballots compatible or
        // aborted. They are aborted if there is a v-blocking set of nodes that
        // prepared a higher ballot
        bool compOrAborted = true;
        for (auto c : stR.pledges.prepare().excepted)
        {
            if (c.value == mBallot.value)
            {
                continue;
            }
            
            auto abortedFilter = [&] (const uint256& nIDA,
                                      const FBAStatement& stA) -> bool
            {
                if (stA.pledges.prepare().prepared) 
                {
                    FBABallot p = *(stA.pledges.prepare().prepared);
                    if (compareBallots(p, c) > 0)
                    {
                        return true;
                    }
                }
                return false;
            };

            if (mFBA->getNode(nIDR)->isVBlocking<FBAStatement>(
                    stR.quorumSetHash,
                    mStatements[ballot][FBAStatementType::PREPARE],
                    abortedFilter))
            {
                continue;
            }

            compOrAborted = false;
        }

        return compOrAborted;
    };

    if (mFBA->getLocalNode()->isQuorumTransitive<FBAStatement>(
            mFBA->getLocalNode()->getQuorumSetHash(),
            mStatements[ballot][FBAStatementType::PREPARE],
            [] (const FBAStatement& s) { return s.quorumSetHash; },
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
    if (mFBA->getLocalNode()->isQuorumTransitive<FBAStatement>(
            mFBA->getLocalNode()->getQuorumSetHash(),
            mStatements[ballot][FBAStatementType::PREPARED],
            [] (const FBAStatement& s) { return s.quorumSetHash; }))
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
    if (mFBA->getLocalNode()->isQuorumTransitive<FBAStatement>(
            mFBA->getLocalNode()->getQuorumSetHash(),
            mStatements[ballot][FBAStatementType::COMMIT],
            [] (const FBAStatement& s) { return s.quorumSetHash; }))
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
    if (mFBA->getLocalNode()->isQuorumTransitive<FBAStatement>(
            mFBA->getLocalNode()->getQuorumSetHash(),
            statements,
            [] (const FBAStatement& s) { return s.quorumSetHash; }))
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
    return mFBA->compareValues(b1.value, b2.value);
}

void
Slot::advanceSlot()
{
    // `advanceSlot` suopports reentrant calls by setting and checking
    // `mInAdvanceSlot`. If a reentrant call is made, `mRunAdvanceSlot` will be
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
            attemptPrepared(mBallot); 
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
                for (auto sit : mStatements)
                {
                    // We consider only the ballots that have a compatible
                    // value
                    if (sit.first.value == bext.value)
                    {
                        // If we have a COMMITTED statement for this ballot and
                        // it is bigger than bext, we bump bext to it.
                        if (!sit.second[FBAStatementType::COMMITTED].empty())
                        {
                            if(compareBallots(bext, sit.first) < 0)
                            {
                                bext = sit.first;
                            }
                        }
                    }
                }

                bumpToBallot(bext);
                attemptCommitted();
            }

            if (isPrepared(b))
            {
                // If a higher ballot has prepared, we can bump to it as our
                // current ballot has become irrelevant (aborted)
                if (compareBallots(b, mBallot) > 0)
                {
                    bumpToBallot(b);
                    mRunAdvanceSlot = true;
                }
                // If it's a smaller ballot we must emit a PREPARED for it.
                // We can't and we won't COMMIT b as `mBallot` only moves
                // monotically.
                else
                {
                    attemptPrepared(b);
                }
            }
        }

    }
    catch(Node::QuorumSetNotFound e)
    {
        auto cb = [this,e] (const FBAQuorumSet& qSet)
        {
            uint256 qSetHash = sha512_256(xdr::xdr_to_msg(qSet));
            if (e.qSetHash() == qSetHash)
            {
                mFBA->getNode(e.nodeID())->cacheQuorumSet(qSet);
                advanceSlot();
            }
        };
        mFBA->retrieveQuorumSet(e.nodeID(), e.qSetHash(), cb);
    }

    mInAdvanceSlot = false;
    if (mRunAdvanceSlot)
    {
        mRunAdvanceSlot = false;
        advanceSlot();
    }
}

}
