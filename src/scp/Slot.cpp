// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "Slot.h"

#include <cassert>
#include "util/types.h"
#include "xdrpp/marshal.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "util/Logging.h"
#include "scp/Node.h"
#include "scp/LocalNode.h"
#include "lib/json/json.h"

namespace stellar
{
using xdr::operator==;
using xdr::operator<;

// Static helper to stringify ballot for logging
std::string
ballotToStr(SCPBallot const& ballot)
{
    std::ostringstream oss;

    uint256 valueHash = sha256(xdr::xdr_to_opaque(ballot.value));

    oss << "(" << ballot.counter << "," << hexAbbrev(valueHash) << ")";
    return oss.str();
}

// Static helper to stringify envelope for logging
std::string
envToStr(SCPEnvelope const& envelope)
{
    std::ostringstream oss;
    oss << "{ENV@" << hexAbbrev(envelope.nodeID) << "|";
    switch (envelope.statement.pledges.type())
    {
    case SCPStatementType::PREPARING:
        oss << "PREPARING";
        break;
    case SCPStatementType::PREPARED:
        oss << "PREPARED";
        break;
    case SCPStatementType::COMMITTING:
        oss << "COMMITTING";
        break;
    case SCPStatementType::COMMITTED:
        oss << "COMMITTED";
        break;
    }

    Hash qSetHash = envelope.statement.quorumSetHash;
    oss << "|" << ballotToStr(envelope.statement.ballot);
    oss << "|" << hexAbbrev(qSetHash) << "}";
    return oss.str();
}

Slot::Slot(uint64 const& slotIndex, SCP* SCP)
    : mSlotIndex(slotIndex)
    , mSCP(SCP)
    , mIsPristine(true)
    , mHeardFromQuorum(true)
    , mIsCommitted(false)
    , mIsExternalized(false)
    , mInAdvanceSlot(false)
    , mRunAdvanceSlot(false)
{
    mBallot.counter = 0;
}

void
Slot::processEnvelope(SCPEnvelope const& envelope,
                      std::function<void(SCP::EnvelopeState)> const& cb)
{
    assert(envelope.statement.slotIndex == mSlotIndex);

    CLOG(DEBUG, "SCP") << "Slot::processEnvelope"
                       << "@" << hexAbbrev(mSCP->getLocalNodeID())
                       << " i: " << mSlotIndex << " " << envToStr(envelope);

    uint256 nodeID = envelope.nodeID;
    SCPStatement statement = envelope.statement;
    SCPBallot b = statement.ballot;
    SCPStatementType t = statement.pledges.type();

    // We copy everything we need as this can be async (no reference).
    auto self = shared_from_this();
    auto value_cb = [b, t, nodeID, statement, cb, self](bool valid)
    {
        // If the value is not valid, we just ignore it.
        if (!valid)
        {
            CLOG(TRACE, "SCP") << "invalid value"
                               << "@" << hexAbbrev(self->mSCP->getLocalNodeID())
                               << " i: " << self->mSlotIndex;

            return cb(SCP::EnvelopeState::INVALID);
        }

        if (!self->mIsCommitted && t == SCPStatementType::PREPARING)
        {
            auto ballot_cb = [b, t, nodeID, statement, cb, self](bool valid)
            {
                // If the ballot is not valid, we just ignore it.
                if (!valid)
                {
                    return cb(SCP::EnvelopeState::INVALID);
                }

                // If a new higher ballot has been issued, let's move on to it.
                if (self->mIsPristine || self->compareBallots(b, self->mBallot) > 0)
                {
                    self->bumpToBallot(b);
                }

                // Finally store the statement and advance the slot if possible.
                self->mStatements[b][t][nodeID] = statement;
                self->advanceSlot();
            };

            self->mSCP->validateBallot(self->mSlotIndex, nodeID, b, ballot_cb);
        }
        else if (!self->mIsCommitted && t == SCPStatementType::PREPARED)
        {
            // A PREPARED statement does not imply any PREPARING so we can go
            // ahead and store it if its value is valid.
            self->mStatements[b][t][nodeID] = statement;
            self->advanceSlot();
        }
        else if (!self->mIsCommitted && t == SCPStatementType::COMMITTING)
        {
            // We accept COMMITTING statements only if we previously saw a valid
            // PREPARED statement for that ballot and all the PREPARING we saw
            // so
            // far have a lower ballot than this one or have that COMMITTING in
            // their B_c.  This prevents node from emitting phony messages too
            // easily.
            bool isPrepared = false;
            for (auto s : self->getNodeStatements(nodeID, SCPStatementType::PREPARED))
            {
                if (self->compareBallots(b, s.ballot) == 0)
                {
                    isPrepared = true;
                }
            }
            for (auto s :
                 self->getNodeStatements(nodeID, SCPStatementType::PREPARING))
            {
                if (self->compareBallots(b, s.ballot) < 0)
                {
                    auto excepted = s.pledges.prepare().excepted;
                    auto it = std::find(excepted.begin(), excepted.end(), b);
                    if (it == excepted.end())
                    {
                        return cb(SCP::EnvelopeState::INVALID);
                    }
                }
            }
            if (isPrepared)
            {
                // Finally store the statement and advance the slot if
                // possible.
                self->mStatements[b][t][nodeID] = statement;
                self->advanceSlot();
            }
            else
            {
                return cb(SCP::EnvelopeState::STATEMENTS_MISSING);
            }
        }
        else if (t == SCPStatementType::COMMITTED)
        {
            // If we already have a COMMITTED statements for this node, we just
            // ignore this one as it is illegal.
            if (self->getNodeStatements(nodeID, SCPStatementType::COMMITTED).size() >
                0)
            {
                CLOG(TRACE, "SCP") << "Node Already Committed"
                                   << "@" << hexAbbrev(self->mSCP->getLocalNodeID())
                                   << " i: " << self->mSlotIndex;
                return cb(SCP::EnvelopeState::INVALID);
            }

            // Finally store the statement and advance the slot if possible.
            self->mStatements[b][t][nodeID] = statement;
            self->advanceSlot();
        }
        else if (self->mIsCommitted)
        {
            // If the slot already COMMITTED and we received another statement,
            // we resend our own COMMITTED message.
            SCPStatement stmt = self->createStatement(SCPStatementType::COMMITTED);

            SCPEnvelope env = self->createEnvelope(stmt);
            self->mSCP->emitEnvelope(env);
        }

        // Finally call the callback saying that this was a valid envelope
        return cb(SCP::EnvelopeState::VALID);
    };

    mSCP->validateValue(mSlotIndex, nodeID, b.value, value_cb);
}

bool
Slot::prepareValue(Value const& value, bool forceBump)
{
    if (mIsCommitted)
    {
        return false;
    }
    if (forceBump || (!mIsPristine &&
                      mSCP->compareValues(mSlotIndex, mBallot.counter,
                                          mBallot.value, value) > 0))
    {
        bumpToBallot(SCPBallot(mBallot.counter + 1, value));
    }
    else
    {
        bumpToBallot(SCPBallot(mBallot.counter, value));
    }

    advanceSlot();
    return true;
}

void
Slot::bumpToBallot(SCPBallot const& ballot)
{
    // `bumpToBallot` should be never called once we committed.
    if (mIsCommitted || mIsExternalized)
    {
        return;
    }

    CLOG(DEBUG, "SCP") << "Slot::bumpToBallot"
                       << "@" << hexAbbrev(mSCP->getLocalNodeID())
                       << " i: " << mSlotIndex << " b: " << ballotToStr(ballot);

    // We shouldn't have emitted any prepare message for this ballot or any
    // other higher ballot.
    for (auto s :
         getNodeStatements(mSCP->getLocalNodeID(), SCPStatementType::PREPARING))
    {
        assert(compareBallots(ballot, s.ballot) >= 0);
    }
    // We should move mBallot monotonically only
    assert(mIsPristine || compareBallots(ballot, mBallot) >= 0);

    mBallot = ballot;

    mIsPristine = false;
    mHeardFromQuorum = false;
}

SCPStatement
Slot::createStatement(SCPStatementType const& type)
{
    SCPStatement statement;

    statement.slotIndex = mSlotIndex;
    statement.ballot = mBallot;
    statement.quorumSetHash = mSCP->getLocalNode()->getQuorumSetHash();
    statement.pledges.type(type);

    return statement;
}

SCPEnvelope
Slot::createEnvelope(SCPStatement const& statement)
{
    SCPEnvelope envelope;

    envelope.nodeID = mSCP->getLocalNodeID();
    envelope.statement = statement;
    mSCP->signEnvelope(envelope);

    return envelope;
}

void
Slot::attemptPreparing()
{
    auto it = mStatements[mBallot][SCPStatementType::PREPARING].find(
        mSCP->getLocalNodeID());
    if (it != mStatements[mBallot][SCPStatementType::PREPARING].end())
    {
        return;
    }
    CLOG(DEBUG, "SCP") << "Slot::attemptPreparing"
                       << "@" << hexAbbrev(mSCP->getLocalNodeID())
                       << " i: " << mSlotIndex
                       << " b: " << ballotToStr(mBallot);

    SCPStatement statement = createStatement(SCPStatementType::PREPARING);

    for (auto s :
         getNodeStatements(mSCP->getLocalNodeID(), SCPStatementType::PREPARED))
    {
        if (s.ballot.value == mBallot.value)
        {
            if (!statement.pledges.prepare().prepared ||
                compareBallots(*(statement.pledges.prepare().prepared),
                               s.ballot) > 0)
            {
                statement.pledges.prepare().prepared.activate() = s.ballot;
            }
        }
    }
    for (auto s : getNodeStatements(mSCP->getLocalNodeID(),
                                    SCPStatementType::COMMITTING))
    {
        statement.pledges.prepare().excepted.push_back(s.ballot);
    }

    mSCP->ballotDidPrepare(mSlotIndex, mBallot);

    SCPEnvelope envelope = createEnvelope(statement);
    auto self = shared_from_this();
    auto cb = [envelope, self](SCP::EnvelopeState s)
    {
        if (s == SCP::EnvelopeState::VALID)
        {
            self->mSCP->emitEnvelope(envelope);
        }
    };
    processEnvelope(envelope, cb);
}

void
Slot::attemptPrepared(SCPBallot const& ballot)
{
    auto it = mStatements[ballot][SCPStatementType::PREPARED].find(
        mSCP->getLocalNodeID());
    if (it != mStatements[ballot][SCPStatementType::PREPARED].end())
    {
        return;
    }
    CLOG(DEBUG, "SCP") << "Slot::attemptPrepared"
                       << "@" << hexAbbrev(mSCP->getLocalNodeID())
                       << " i: " << mSlotIndex << " b: " << ballotToStr(ballot);

    SCPStatement statement = createStatement(SCPStatementType::PREPARED);
    statement.ballot = ballot;

    SCPEnvelope envelope = createEnvelope(statement);
    auto self = shared_from_this();
    auto cb = [envelope, self](SCP::EnvelopeState s)
    {
        if (s == SCP::EnvelopeState::VALID)
        {
            self->mSCP->emitEnvelope(envelope);
        }
    };
    processEnvelope(envelope, cb);
}

void
Slot::attemptCommitting()
{
    auto it = mStatements[mBallot][SCPStatementType::COMMITTING].find(
        mSCP->getLocalNodeID());
    if (it != mStatements[mBallot][SCPStatementType::COMMITTING].end())
    {
        return;
    }
    CLOG(DEBUG, "SCP") << "Slot::attemptCommitting"
                       << "@" << hexAbbrev(mSCP->getLocalNodeID())
                       << " i: " << mSlotIndex
                       << " b: " << ballotToStr(mBallot);

    SCPStatement statement = createStatement(SCPStatementType::COMMITTING);

    mSCP->ballotDidCommit(mSlotIndex, mBallot);

    SCPEnvelope envelope = createEnvelope(statement);
    auto self = shared_from_this();
    auto cb = [envelope, self](SCP::EnvelopeState s)
    {
        if (s == SCP::EnvelopeState::VALID)
        {
            self->mSCP->emitEnvelope(envelope);
        }
    };
    processEnvelope(envelope, cb);
}

void
Slot::attemptCommitted()
{
    auto it = mStatements[mBallot][SCPStatementType::COMMITTED].find(
        mSCP->getLocalNodeID());
    if (it != mStatements[mBallot][SCPStatementType::COMMITTED].end())
    {
        return;
    }
    CLOG(DEBUG, "SCP") << "Slot::attemptCommitted"
                       << "@" << hexAbbrev(mSCP->getLocalNodeID())
                       << " i: " << mSlotIndex
                       << " b: " << ballotToStr(mBallot);

    SCPStatement statement = createStatement(SCPStatementType::COMMITTED);

    mIsCommitted = true;
    mSCP->ballotDidCommitted(mSlotIndex, mBallot);

    SCPEnvelope envelope = createEnvelope(statement);
    auto self = shared_from_this();
    auto cb = [envelope, self](SCP::EnvelopeState s)
    {
        if (s == SCP::EnvelopeState::VALID)
        {
            self->mSCP->emitEnvelope(envelope);
        }
    };
    processEnvelope(envelope, cb);
}

void
Slot::attemptExternalize()
{
    if (mIsExternalized)
    {
        return;
    }
    CLOG(DEBUG, "SCP") << "Slot::attemptExternalize"
                       << "@" << hexAbbrev(mSCP->getLocalNodeID())
                       << " i: " << mSlotIndex
                       << " b: " << ballotToStr(mBallot);

    mIsExternalized = true;

    mSCP->valueExternalized(mSlotIndex, mBallot.value);
}

bool
Slot::isPrepared(SCPBallot const& ballot)
{
    // Checks if we haven't already emitted PREPARED b
    auto it = mStatements[ballot][SCPStatementType::PREPARED].find(
        mSCP->getLocalNodeID());
    if (it != mStatements[ballot][SCPStatementType::PREPARED].end())
    {
        return true;
    }

    // Checks if there is a v-blocking set of nodes that accepted the PREPARING
    // statements (this is an optimization).
    if (mSCP->getLocalNode()->isVBlocking<SCPStatement>(
            mSCP->getLocalNode()->getQuorumSetHash(),
            mStatements[ballot][SCPStatementType::PREPARED]))
    {
        return true;
    }

    // Check if we can establish the pledges for a transitive quorum.
    auto ratifyFilter =
        [&](uint256 const& nIDR, SCPStatement const& stR) -> bool
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

            auto abortedFilter =
                [&](uint256 const& nIDA, SCPStatement const& stA) -> bool
            {
                if (stA.pledges.prepare().prepared)
                {
                    SCPBallot p = *(stA.pledges.prepare().prepared);
                    if (compareBallots(p, c) > 0)
                    {
                        return true;
                    }
                }
                return false;
            };

            if (mSCP->getNode(nIDR)->isVBlocking<SCPStatement>(
                    stR.quorumSetHash,
                    mStatements[ballot][SCPStatementType::PREPARING],
                    abortedFilter))
            {
                continue;
            }

            compOrAborted = false;
        }

        return compOrAborted;
    };

    if (mSCP->getLocalNode()->isQuorumTransitive<SCPStatement>(
            mSCP->getLocalNode()->getQuorumSetHash(),
            mStatements[ballot][SCPStatementType::PREPARING],
            [](SCPStatement const& s)
            {
                return s.quorumSetHash;
            },
            ratifyFilter))
    {
        return true;
    }

    return false;
}

bool
Slot::isPreparedConfirmed(SCPBallot const& ballot)
{
    // Checks if we haven't already emitted COMMITTING b
    auto it = mStatements[ballot][SCPStatementType::COMMITTING].find(
        mSCP->getLocalNodeID());
    if (it != mStatements[ballot][SCPStatementType::COMMITTING].end())
    {
        return true;
    }

    // Checks if there is a transitive quorum that accepted the PREPARING
    // statements for the local node.
    if (mSCP->getLocalNode()->isQuorumTransitive<SCPStatement>(
            mSCP->getLocalNode()->getQuorumSetHash(),
            mStatements[ballot][SCPStatementType::PREPARED],
            [](SCPStatement const& s)
            {
                return s.quorumSetHash;
            }))
    {
        return true;
    }
    return false;
}

bool
Slot::isCommitted(SCPBallot const& ballot)
{
    // Checks if we haven't already emitted COMMITTED b
    auto it = mStatements[ballot][SCPStatementType::COMMITTED].find(
        mSCP->getLocalNodeID());
    if (it != mStatements[ballot][SCPStatementType::COMMITTED].end())
    {
        return true;
    }

    // Check if we can establish the pledges for a transitive quorum.
    if (mSCP->getLocalNode()->isQuorumTransitive<SCPStatement>(
            mSCP->getLocalNode()->getQuorumSetHash(),
            mStatements[ballot][SCPStatementType::COMMITTING],
            [](SCPStatement const& s)
            {
                return s.quorumSetHash;
            }))
    {
        return true;
    }

    return false;
}

bool
Slot::isCommittedConfirmed(Value const& value)
{
    std::map<uint256, SCPStatement> statements;
    for (auto it : mStatements)
    {
        if (it.first.value == value)
        {
            for (auto sit : it.second[SCPStatementType::COMMITTED])
            {
                statements[sit.first] = sit.second;
            }
        }
    }

    // Checks if there is a transitive quorum that accepted the COMMITTING
    // statement for the local node.
    if (mSCP->getLocalNode()->isQuorumTransitive<SCPStatement>(
            mSCP->getLocalNode()->getQuorumSetHash(), statements,
            [](SCPStatement const& s)
            {
                return s.quorumSetHash;
            }))
    {
        return true;
    }
    return false;
}

std::vector<SCPStatement>
Slot::getNodeStatements(uint256 const& nodeID, SCPStatementType const& type)
{
    std::vector<SCPStatement> statements;
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
Slot::compareBallots(SCPBallot const& b1, SCPBallot const& b2)
{
    if (b1.counter < b2.counter)
    {
        return -1;
    }
    if (b2.counter < b1.counter)
    {
        return 1;
    }
    return mSCP->compareValues(mSlotIndex, b1.counter, b1.value, b2.value);
}

void
Slot::advanceSlot()
{
    // `advanceSlot` supports reentrant calls by setting and checking
    // `mInAdvanceSlot`. If a reentrant call is made, `mRunAdvanceSlot` will be
    // set and `advanceSlot` will be called again after it is done executing.
    if (mInAdvanceSlot)
    {
        CLOG(DEBUG, "SCP") << "already in advanceSlot"
                           << "@" << hexAbbrev(mSCP->getLocalNodeID())
                           << " i: " << mSlotIndex
                           << " b: " << ballotToStr(mBallot);

        mRunAdvanceSlot = true;
        return;
    }
    mInAdvanceSlot = true;

    try
    {
        CLOG(DEBUG, "SCP") << "Slot::advanceSlot"
                           << "@" << hexAbbrev(mSCP->getLocalNodeID())
                           << " i: " << mSlotIndex
                           << " b: " << ballotToStr(mBallot);

        // If we're pristine, we haven't set `mBallot` yet so we just skip
        // to the search for conditions to bump our ballot
        if (!mIsPristine)
        {
            if (!mIsCommitted)
            {
                attemptPreparing();

                if (isPrepared(mBallot))
                {
                    attemptPrepared(mBallot);
                }

                // If our current ballot is prepared confirmed we can move onto
                // the commit phase
                if (isPreparedConfirmed(mBallot))
                {
                    attemptCommitting();

                    if (isCommitted(mBallot))
                    {
                        attemptCommitted();
                    }
                }
            }
            else
            {
                // If our current ballot is committed and we can confirm the
                // value then we externalize
                if (isCommittedConfirmed(mBallot.value))
                {
                    attemptExternalize();
                }
            }
        }

        // We loop on all known ballots to check if there are conditions that
        // should make us bump our current ballot
        for (auto it : mStatements)
        {
            // None of this apply if we committed or externalized
            if (mIsCommitted || mIsExternalized)
            {
                break;
            }

            SCPBallot b = it.first;

            CLOG(DEBUG, "SCP") << "Slot::advanceSlot::tryBumping"
                               << "@" << hexAbbrev(mSCP->getLocalNodeID())
                               << " i: " << mSlotIndex
                               << " b: " << ballotToStr(mBallot);

            // If we could externalize by moving on to a given value we bump
            // our ballot to the appropriate one
            if (isCommittedConfirmed(b.value))
            {
                assert(!mIsCommitted || mBallot.value == b.value);

                // We look for the smallest ballot that is bigger than all the
                // COMMITTED message we saw for the value and our own current
                // ballot.
                SCPBallot bext = SCPBallot(mBallot.counter, b.value);
                if (compareBallots(bext, mBallot) < 0)
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
                        if (!sit.second[SCPStatementType::COMMITTED].empty())
                        {
                            if (compareBallots(bext, sit.first) < 0)
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
                // We can't and we won't COMMITTING b as `mBallot` only moves
                // monotonically.
                else
                {
                    attemptPrepared(b);
                }
            }
        }

        // Check if we can call `ballotDidHearFromQuorum`
        if (!mHeardFromQuorum)
        {
            std::map<uint256, SCPStatement> allStatements;
            for (auto tp : mStatements[mBallot])
            {
                for (auto np : tp.second)
                {
                    allStatements[np.first] = np.second;
                }
            }
            if (mSCP->getLocalNode()->isQuorumTransitive<SCPStatement>(
                    mSCP->getLocalNode()->getQuorumSetHash(), allStatements,
                    [](SCPStatement const& s)
                    {
                        return s.quorumSetHash;
                    }))
            {
                mHeardFromQuorum = true;
                mSCP->ballotDidHearFromQuorum(mSlotIndex, mBallot);
            }
        }
    }
    catch (Node::QuorumSetNotFound e)
    {
        auto self = shared_from_this();
        auto cb = [self, e](SCPQuorumSet const& qSet)
        {
            uint256 qSetHash = sha256(xdr::xdr_to_opaque(qSet));
            if (e.qSetHash() == qSetHash)
            {
                self->mSCP->getNode(e.nodeID())->cacheQuorumSet(qSet);
                self->advanceSlot();
            }
        };
        mSCP->retrieveQuorumSet(e.nodeID(), e.qSetHash(), cb);
    }

    mInAdvanceSlot = false;
    if (mRunAdvanceSlot)
    {
        mRunAdvanceSlot = false;
        advanceSlot();
    }
}

size_t
Slot::getStatementCount() const
{
    return mStatements.size();
}

void
Slot::dumpInfo(Json::Value& ret)
{
    Json::Value slotValue;
    slotValue["index"] = static_cast<int>(mSlotIndex);
    slotValue["pristine"] = mIsPristine;
    slotValue["heard"] = mHeardFromQuorum;
    slotValue["committed"] = mIsCommitted;
    slotValue["pristine"] = mIsExternalized;
    slotValue["ballot"] = ballotToStr(mBallot);

    std::string stateStrTable[] = {"PREPARING", "PREPARED", "COMMITTING",
                                   "COMMITTED"};

    int count = 0;
    for (auto& item : mStatements)
    {
        for (auto& mapItem : item.second)
        {
            for (auto& stateItem : mapItem.second)
            {
                // ballot, node, qset, state
                std::ostringstream output;
                output << "b:" << ballotToStr(item.first)
                       << " n:" << hexAbbrev(stateItem.first)
                       << " q:" << hexAbbrev(stateItem.second.quorumSetHash)
                       << " ,"
                       << stateStrTable[static_cast<int>(stateItem.second.pledges.type())];
                slotValue["statements"][count++] = output.str();
            }
        }
    }

    ret["slot"].append(slotValue);
}
}
