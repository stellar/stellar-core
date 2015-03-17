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
#include "scp/Node.h"
#include "scp/LocalNode.h"

namespace stellar
{
using xdr::operator==;
using xdr::operator<;

// Static helper to stringify ballot for logging
std::string
ballotToStr(const SCPBallot& ballot)
{
    std::ostringstream oss;

    uint256 valueHash = sha256(xdr::xdr_to_opaque(ballot.value));

    oss << "(" << ballot.counter << "," << binToHex(valueHash).substr(0, 6)
        << ")";
    return oss.str();
}

// Static helper to stringify envelope for logging
std::string
envToStr(const SCPEnvelope& envelope)
{
    std::ostringstream oss;
    oss << "{ENV@" << binToHex(envelope.nodeID).substr(0, 6) << "|";
    switch (envelope.statement.pledges.type())
    {
    case SCPStatementType::PREPARE:
        oss << "PREPARE";
        break;
    case SCPStatementType::PREPARED:
        oss << "PREPARED";
        break;
    case SCPStatementType::COMMIT:
        oss << "COMMIT";
        break;
    case SCPStatementType::COMMITTED:
        oss << "COMMITTED";
        break;
    }

    Hash qSetHash = envelope.statement.quorumSetHash;
    oss << "|" << ballotToStr(envelope.statement.ballot);
    oss << "|" << binToHex(qSetHash).substr(0, 6) << "}";
    return oss.str();
}

Slot::Slot(const uint64& slotIndex, SCP* SCP)
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
Slot::processEnvelope(const SCPEnvelope& envelope,
                      std::function<void(SCP::EnvelopeState)> const& cb)
{
    assert(envelope.statement.slotIndex == mSlotIndex);

    CLOG(DEBUG, "SCP") << "Slot::processEnvelope"
                       << "@" << binToHex(mSCP->getLocalNodeID()).substr(0, 6)
                       << " i: " << mSlotIndex << " " << envToStr(envelope);

    uint256 nodeID = envelope.nodeID;
    SCPStatement statement = envelope.statement;
    SCPBallot b = statement.ballot;
    SCPStatementType t = statement.pledges.type();

    // We copy everything we need as this can be async (no reference).
    auto value_cb = [b, t, nodeID, statement, cb, this](bool valid)
    {
        // If the value is not valid, we just ignore it.
        if (!valid)
        {
            return cb(SCP::EnvelopeState::INVALID);
        }

        if (!mIsCommitted && t == SCPStatementType::PREPARE)
        {
            auto ballot_cb = [b, t, nodeID, statement, cb, this](bool valid)
            {
                // If the ballot is not valid, we just ignore it.
                if (!valid)
                {
                    return cb(SCP::EnvelopeState::INVALID);
                }

                // If a new higher ballot has been issued, let's move on to it.
                if (mIsPristine || compareBallots(b, mBallot) > 0)
                {
                    bumpToBallot(b);
                }

                // Finally store the statement and advance the slot if possible.
                mStatements[b][t][nodeID] = statement;
                advanceSlot();
            };

            mSCP->validateBallot(mSlotIndex, nodeID, b, ballot_cb);
        }
        else if (!mIsCommitted && t == SCPStatementType::PREPARED)
        {
            // A PREPARED statement does not imply any PREPARE so we can go
            // ahead and store it if its value is valid.
            mStatements[b][t][nodeID] = statement;
            advanceSlot();
        }
        else if (!mIsCommitted && t == SCPStatementType::COMMIT)
        {
            // We accept COMMIT statements only if we previously saw a valid
            // PREPARED statement for that ballot and all the PREPARE we saw so
            // far have a lower ballot than this one or have that COMMIT in
            // their B_c.  This prevents node from emitting phony messages too
            // easily.
            bool isPrepared = false;
            for (auto s : getNodeStatements(nodeID, SCPStatementType::PREPARED))
            {
                if (compareBallots(b, s.ballot) == 0)
                {
                    isPrepared = true;
                }
            }
            for (auto s : getNodeStatements(nodeID, SCPStatementType::PREPARE))
            {
                if (compareBallots(b, s.ballot) < 0)
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
                mStatements[b][t][nodeID] = statement;
                advanceSlot();
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
            if (getNodeStatements(nodeID, SCPStatementType::COMMITTED).size() >
                0)
            {
                return cb(SCP::EnvelopeState::INVALID);
            }

            // Finally store the statement and advance the slot if possible.
            mStatements[b][t][nodeID] = statement;
            advanceSlot();
        }
        else if (mIsCommitted)
        {
            // If the slot already COMMITTED and we received another statement,
            // we resend our own COMMITTED message.
            SCPStatement stmt = createStatement(SCPStatementType::COMMITTED);

            SCPEnvelope env = createEnvelope(stmt);
            mSCP->emitEnvelope(env);
        }

        // Finally call the callback saying that this was a valid envelope
        return cb(SCP::EnvelopeState::VALID);
    };

    mSCP->validateValue(mSlotIndex, nodeID, b.value, value_cb);
}

bool
Slot::prepareValue(const Value& value, bool forceBump)
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
Slot::bumpToBallot(const SCPBallot& ballot)
{
    // `bumpToBallot` should be never called once we committed.
    assert(!mIsCommitted && !mIsExternalized);

    CLOG(DEBUG, "SCP") << "Slot::bumpToBallot"
                       << "@" << binToHex(mSCP->getLocalNodeID()).substr(0, 6)
                       << " i: " << mSlotIndex << " b: " << ballotToStr(ballot);

    // We shouldn't have emitted any prepare message for this ballot or any
    // other higher ballot.
    for (auto s :
         getNodeStatements(mSCP->getLocalNodeID(), SCPStatementType::PREPARE))
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
Slot::createStatement(const SCPStatementType& type)
{
    SCPStatement statement;

    statement.slotIndex = mSlotIndex;
    statement.ballot = mBallot;
    statement.quorumSetHash = mSCP->getLocalNode()->getQuorumSetHash();
    statement.pledges.type(type);

    return statement;
}

SCPEnvelope
Slot::createEnvelope(const SCPStatement& statement)
{
    SCPEnvelope envelope;

    envelope.nodeID = mSCP->getLocalNodeID();
    envelope.statement = statement;
    mSCP->signEnvelope(envelope);

    return envelope;
}

void
Slot::attemptPrepare()
{
    auto it = mStatements[mBallot][SCPStatementType::PREPARE].find(
        mSCP->getLocalNodeID());
    if (it != mStatements[mBallot][SCPStatementType::PREPARE].end())
    {
        return;
    }
    CLOG(DEBUG, "SCP") << "Slot::attemptPrepare"
                       << "@" << binToHex(mSCP->getLocalNodeID()).substr(0, 6)
                       << " i: " << mSlotIndex
                       << " b: " << ballotToStr(mBallot);

    SCPStatement statement = createStatement(SCPStatementType::PREPARE);

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
    for (auto s :
         getNodeStatements(mSCP->getLocalNodeID(), SCPStatementType::COMMIT))
    {
        statement.pledges.prepare().excepted.push_back(s.ballot);
    }

    mSCP->ballotDidPrepare(mSlotIndex, mBallot);

    SCPEnvelope envelope = createEnvelope(statement);
    auto cb = [envelope, this](const SCP::EnvelopeState& s)
    {
        if (s == SCP::EnvelopeState::VALID)
        {
            mSCP->emitEnvelope(envelope);
        }
    };
    processEnvelope(envelope, cb);
}

void
Slot::attemptPrepared(const SCPBallot& ballot)
{
    auto it = mStatements[ballot][SCPStatementType::PREPARED].find(
        mSCP->getLocalNodeID());
    if (it != mStatements[ballot][SCPStatementType::PREPARED].end())
    {
        return;
    }
    CLOG(DEBUG, "SCP") << "Slot::attemptPrepared"
                       << "@" << binToHex(mSCP->getLocalNodeID()).substr(0, 6)
                       << " i: " << mSlotIndex << " b: " << ballotToStr(ballot);

    SCPStatement statement = createStatement(SCPStatementType::PREPARED);
    statement.ballot = ballot;

    SCPEnvelope envelope = createEnvelope(statement);
    auto cb = [envelope, this](SCP::EnvelopeState s)
    {
        if (s == SCP::EnvelopeState::VALID)
        {
            mSCP->emitEnvelope(envelope);
        }
    };
    processEnvelope(envelope, cb);
}

void
Slot::attemptCommit()
{
    auto it = mStatements[mBallot][SCPStatementType::COMMIT].find(
        mSCP->getLocalNodeID());
    if (it != mStatements[mBallot][SCPStatementType::COMMIT].end())
    {
        return;
    }
    CLOG(DEBUG, "SCP") << "Slot::attemptCommit"
                       << "@" << binToHex(mSCP->getLocalNodeID()).substr(0, 6)
                       << " i: " << mSlotIndex
                       << " b: " << ballotToStr(mBallot);

    SCPStatement statement = createStatement(SCPStatementType::COMMIT);

    mSCP->ballotDidCommit(mSlotIndex, mBallot);

    SCPEnvelope envelope = createEnvelope(statement);
    auto cb = [envelope, this](SCP::EnvelopeState s)
    {
        if (s == SCP::EnvelopeState::VALID)
        {
            mSCP->emitEnvelope(envelope);
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
                       << "@" << binToHex(mSCP->getLocalNodeID()).substr(0, 6)
                       << " i: " << mSlotIndex
                       << " b: " << ballotToStr(mBallot);

    SCPStatement statement = createStatement(SCPStatementType::COMMITTED);

    mIsCommitted = true;
    mSCP->ballotDidCommitted(mSlotIndex, mBallot);

    SCPEnvelope envelope = createEnvelope(statement);
    auto cb = [envelope, this](SCP::EnvelopeState s)
    {
        if (s == SCP::EnvelopeState::VALID)
        {
            mSCP->emitEnvelope(envelope);
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
                       << "@" << binToHex(mSCP->getLocalNodeID()).substr(0, 6)
                       << " i: " << mSlotIndex
                       << " b: " << ballotToStr(mBallot);

    mIsExternalized = true;

    mSCP->valueExternalized(mSlotIndex, mBallot.value);
}

bool
Slot::isPrepared(const SCPBallot& ballot)
{
    // Checks if we haven't already emitted PREPARED b
    auto it = mStatements[ballot][SCPStatementType::PREPARED].find(
        mSCP->getLocalNodeID());
    if (it != mStatements[ballot][SCPStatementType::PREPARED].end())
    {
        return true;
    }

    // Checks if there is a v-blocking set of nodes that accepted the PREPARE
    // statements (this is an optimization).
    if (mSCP->getLocalNode()->isVBlocking<SCPStatement>(
            mSCP->getLocalNode()->getQuorumSetHash(),
            mStatements[ballot][SCPStatementType::PREPARED]))
    {
        return true;
    }

    // Check if we can establish the pledges for a transitive quorum.
    auto ratifyFilter = [&](const uint256& nIDR, const SCPStatement& stR)
                            -> bool
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
                [&](const uint256& nIDA, const SCPStatement& stA) -> bool
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
                    mStatements[ballot][SCPStatementType::PREPARE],
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
            mStatements[ballot][SCPStatementType::PREPARE],
            [](const SCPStatement& s)
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
Slot::isPreparedConfirmed(const SCPBallot& ballot)
{
    // Checks if we haven't already emitted COMMIT b
    auto it = mStatements[ballot][SCPStatementType::COMMIT].find(
        mSCP->getLocalNodeID());
    if (it != mStatements[ballot][SCPStatementType::COMMIT].end())
    {
        return true;
    }

    // Checks if there is a transitive quorum that accepted the PREPARE
    // statements for the local node.
    if (mSCP->getLocalNode()->isQuorumTransitive<SCPStatement>(
            mSCP->getLocalNode()->getQuorumSetHash(),
            mStatements[ballot][SCPStatementType::PREPARED],
            [](const SCPStatement& s)
            {
                return s.quorumSetHash;
            }))
    {
        return true;
    }
    return false;
}

bool
Slot::isCommitted(const SCPBallot& ballot)
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
            mStatements[ballot][SCPStatementType::COMMIT],
            [](const SCPStatement& s)
            {
                return s.quorumSetHash;
            }))
    {
        return true;
    }

    return false;
}

bool
Slot::isCommittedConfirmed(const Value& value)
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

    // Checks if there is a transitive quorum that accepted the COMMIT
    // statement for the local node.
    if (mSCP->getLocalNode()->isQuorumTransitive<SCPStatement>(
            mSCP->getLocalNode()->getQuorumSetHash(), statements,
            [](const SCPStatement& s)
            {
                return s.quorumSetHash;
            }))
    {
        return true;
    }
    return false;
}

std::vector<SCPStatement>
Slot::getNodeStatements(const uint256& nodeID, const SCPStatementType& type)
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
Slot::compareBallots(const SCPBallot& b1, const SCPBallot& b2)
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
        mRunAdvanceSlot = true;
        return;
    }
    mInAdvanceSlot = true;

    try
    {
        CLOG(DEBUG, "SCP") << "Slot::advanceSlot"
                           << "@"
                           << binToHex(mSCP->getLocalNodeID()).substr(0, 6)
                           << " i: " << mSlotIndex
                           << " b: " << ballotToStr(mBallot);

        // If we're pristine, we haven't set `mBallot` yet so we just skip
        // to the search for conditions to bump our ballot
        if (!mIsPristine)
        {
            if (!mIsCommitted)
            {
                attemptPrepare();

                if (isPrepared(mBallot))
                {
                    attemptPrepared(mBallot);
                }

                // If our current ballot is prepared confirmed we can move onto
                // the commit phase
                if (isPreparedConfirmed(mBallot))
                {
                    attemptCommit();

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

            CLOG(DEBUG, "SCP")
                << "Slot::advanceSlot::tryBumping"
                << "@" << binToHex(mSCP->getLocalNodeID()).substr(0, 6)
                << " i: " << mSlotIndex << " b: " << ballotToStr(mBallot);

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
                // We can't and we won't COMMIT b as `mBallot` only moves
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
                    [](const SCPStatement& s)
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
        auto cb = [this, e](const SCPQuorumSet& qSet)
        {
            uint256 qSetHash = sha256(xdr::xdr_to_opaque(qSet));
            if (e.qSetHash() == qSetHash)
            {
                mSCP->getNode(e.nodeID())->cacheQuorumSet(qSet);
                advanceSlot();
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
}
