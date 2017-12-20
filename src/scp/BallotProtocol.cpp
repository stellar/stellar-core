// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "BallotProtocol.h"

#include "Slot.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "lib/json/json.h"
#include "scp/LocalNode.h"
#include "scp/QuorumSetUtils.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/make_unique.h"
#include "util/types.h"
#include "xdrpp/marshal.h"
#include <functional>

namespace stellar
{
using xdr::operator==;
using xdr::operator<;
using namespace std::placeholders;

// max number of transitions that can occur from processing one message
static const int MAX_ADVANCE_SLOT_RECURSION = 50;

BallotProtocol::BallotProtocol(Slot& slot)
    : mSlot(slot)
    , mHeardFromQuorum(true)
    , mPhase(SCP_PHASE_PREPARE)
    , mCurrentMessageLevel(0)
{
}

bool
BallotProtocol::isNewerStatement(NodeID const& nodeID, SCPStatement const& st)
{
    auto oldp = mLatestEnvelopes.find(nodeID);
    bool res = false;

    if (oldp == mLatestEnvelopes.end())
    {
        res = true;
    }
    else
    {
        res = isNewerStatement(oldp->second.statement, st);
    }
    return res;
}

bool
BallotProtocol::isNewerStatement(SCPStatement const& oldst,
                                 SCPStatement const& st)
{
    bool res = false;

    // total ordering described in SCP paper.
    auto t = st.pledges.type();

    // statement type (PREPARE < CONFIRM < EXTERNALIZE)
    if (oldst.pledges.type() != t)
    {
        res = (oldst.pledges.type() < t);
    }
    else
    {
        // can't have duplicate EXTERNALIZE statements
        if (t == SCPStatementType::SCP_ST_EXTERNALIZE)
        {
            res = false;
        }
        else if (t == SCPStatementType::SCP_ST_CONFIRM)
        {
            // sorted by (b, p, p', h) (p' = 0 implicitely)
            auto const& oldC = oldst.pledges.confirm();
            auto const& c = st.pledges.confirm();
            int compBallot = compareBallots(oldC.ballot, c.ballot);
            if (compBallot < 0)
            {
                res = true;
            }
            else if (compBallot == 0)
            {
                if (oldC.nPrepared == c.nPrepared)
                {
                    res = (oldC.nH < c.nH);
                }
                else
                {
                    res = (oldC.nPrepared < c.nPrepared);
                }
            }
        }
        else
        {
            // Lexicographical order between PREPARE statements:
            // (b, p, p', h)
            auto const& oldPrep = oldst.pledges.prepare();
            auto const& prep = st.pledges.prepare();

            int compBallot = compareBallots(oldPrep.ballot, prep.ballot);
            if (compBallot < 0)
            {
                res = true;
            }
            else if (compBallot == 0)
            {
                compBallot = compareBallots(oldPrep.prepared, prep.prepared);
                if (compBallot < 0)
                {
                    res = true;
                }
                else if (compBallot == 0)
                {
                    compBallot = compareBallots(oldPrep.preparedPrime,
                                                prep.preparedPrime);
                    if (compBallot < 0)
                    {
                        res = true;
                    }
                    else if (compBallot == 0)
                    {
                        res = (oldPrep.nH < prep.nH);
                    }
                }
            }
        }
    }

    return res;
}

void
BallotProtocol::recordEnvelope(SCPEnvelope const& env)
{
    auto const& st = env.statement;
    auto oldp = mLatestEnvelopes.find(st.nodeID);
    if (oldp == mLatestEnvelopes.end())
    {
        mLatestEnvelopes.insert(std::make_pair(st.nodeID, env));
    }
    else
    {
        oldp->second = env;
    }
    mSlot.recordStatement(env.statement);
}

SCP::EnvelopeState
BallotProtocol::processEnvelope(SCPEnvelope const& envelope, bool self)
{
    SCP::EnvelopeState res = SCP::EnvelopeState::INVALID;
    dbgAssert(envelope.statement.slotIndex == mSlot.getSlotIndex());

    SCPStatement const& statement = envelope.statement;
    NodeID const& nodeID = statement.nodeID;

    if (!isStatementSane(statement, self))
    {
        if (self)
        {
            CLOG(ERROR, "SCP") << "not sane statement from self, skipping "
                               << "  e: " << mSlot.getSCP().envToStr(envelope);
        }

        return SCP::EnvelopeState::INVALID;
    }

    if (!isNewerStatement(nodeID, statement))
    {
        if (self)
        {
            CLOG(ERROR, "SCP") << "stale statement from self, skipping "
                               << "  e: " << mSlot.getSCP().envToStr(envelope);
        }
        else
        {
            CLOG(TRACE, "SCP") << "stale statement, skipping "
                               << " i: " << mSlot.getSlotIndex();
        }

        return SCP::EnvelopeState::INVALID;
    }

    auto validationRes = validateValues(statement);
    if (validationRes != SCPDriver::kInvalidValue)
    {
        bool processed = false;

        if (mPhase != SCP_PHASE_EXTERNALIZE)
        {
            if (validationRes == SCPDriver::kMaybeValidValue)
            {
                mSlot.setFullyValidated(false);
            }

            recordEnvelope(envelope);
            processed = true;
            advanceSlot(statement);
            res = SCP::EnvelopeState::VALID;
        }

        if (!processed)
        {
            // note: this handles also our own messages
            // in particular our final EXTERNALIZE message
            if (mPhase == SCP_PHASE_EXTERNALIZE &&
                mCommit->value == getWorkingBallot(statement).value)
            {
                recordEnvelope(envelope);
                res = SCP::EnvelopeState::VALID;
            }
            else
            {
                if (self)
                {
                    CLOG(ERROR, "SCP")
                        << "externalize statement with invalid value from "
                           "self, skipping "
                        << "  e: " << mSlot.getSCP().envToStr(envelope);
                }

                res = SCP::EnvelopeState::INVALID;
            }
        }
    }
    else
    {
        // If the value is not valid, we just ignore it.
        if (self)
        {
            CLOG(ERROR, "SCP") << "invalid value from self, skipping "
                               << "  e: " << mSlot.getSCP().envToStr(envelope);
        }
        else
        {
            CLOG(TRACE, "SCP") << "invalid value "
                               << " i: " << mSlot.getSlotIndex();
        }

        res = SCP::EnvelopeState::INVALID;
    }
    return res;
}

bool
BallotProtocol::isStatementSane(SCPStatement const& st, bool self)
{

    bool res = isQuorumSetSane(*mSlot.getQuorumSetFromStatement(st), false);
    if (!res)
    {
        CLOG(DEBUG, "SCP") << "Invalid quorum set received";
        return false;
    }

    switch (st.pledges.type())
    {
    case SCPStatementType::SCP_ST_PREPARE:
    {
        auto const& p = st.pledges.prepare();
        // self is allowed to have b = 0 (as long as it never gets emitted)
        bool isOK = self || p.ballot.counter > 0;

        isOK = isOK &&
               ((!p.preparedPrime || !p.prepared) ||
                (areBallotsLessAndIncompatible(*p.preparedPrime, *p.prepared)));

        isOK =
            isOK && (p.nH == 0 || (p.prepared && p.nH <= p.prepared->counter));

        // c != 0 -> c <= h <= b
        isOK = isOK && (p.nC == 0 || (p.nH != 0 && p.ballot.counter >= p.nH &&
                                      p.nH >= p.nC));

        if (!isOK)
        {
            CLOG(DEBUG, "SCP") << "Malformed PREPARE message";
            res = false;
        }
    }
    break;
    case SCPStatementType::SCP_ST_CONFIRM:
    {
        auto const& c = st.pledges.confirm();
        // c <= h <= b
        res = c.ballot.counter > 0;
        res = res && (c.nH <= c.ballot.counter);
        res = res && (c.nCommit <= c.nH);
        if (!res)
        {
            CLOG(DEBUG, "SCP") << "Malformed CONFIRM message";
        }
    }
    break;
    case SCPStatementType::SCP_ST_EXTERNALIZE:
    {
        auto const& e = st.pledges.externalize();

        res = e.commit.counter > 0;
        res = res && e.nH >= e.commit.counter;

        if (!res)
        {
            CLOG(DEBUG, "SCP") << "Malformed EXTERNALIZE message";
        }
    }
    break;
    default:
        dbgAbort();
    }

    return res;
}

bool
BallotProtocol::abandonBallot(uint32 n)
{
    CLOG(DEBUG, "SCP") << "BallotProtocol::abandonBallot";
    bool res = false;
    Value v = mSlot.getLatestCompositeCandidate();
    if (v.empty())
    {
        if (mCurrentBallot)
        {
            v = mCurrentBallot->value;
        }
    }
    if (!v.empty())
    {
        if (n == 0)
        {
            res = bumpState(v, true);
        }
        else
        {
            res = bumpState(v, n);
        }
    }
    return res;
}

bool
BallotProtocol::bumpState(Value const& value, bool force)
{
    uint32 n;
    if (!force && mCurrentBallot)
    {
        return false;
    }

    SCPBallot newb;

    n = mCurrentBallot ? (mCurrentBallot->counter + 1) : 1;

    return bumpState(value, n);
}

bool
BallotProtocol::bumpState(Value const& value, uint32 n)
{
    if (mPhase != SCP_PHASE_PREPARE && mPhase != SCP_PHASE_CONFIRM)
    {
        return false;
    }

    SCPBallot newb;

    newb.counter = n;

    if (mHighBallot)
    {
        // can only bump the counter if we committed to something already
        newb.value = mHighBallot->value;
    }
    else
    {
        newb.value = value;
    }

    if (Logging::logDebug("SCP"))
        CLOG(DEBUG, "SCP") << "BallotProtocol::bumpState"
                           << " i: " << mSlot.getSlotIndex()
                           << " v: " << mSlot.getSCP().ballotToStr(newb);

    bool updated = updateCurrentValue(newb);

    if (updated)
    {
        mSlot.getSCPDriver().startedBallotProtocol(mSlot.getSlotIndex(), newb);
        emitCurrentStateStatement();
    }

    return updated;
}

// updates the local state based to the specificed ballot
// (that could be a prepared ballot) enforcing invariants
bool
BallotProtocol::updateCurrentValue(SCPBallot const& ballot)
{
    if (mPhase != SCP_PHASE_PREPARE && mPhase != SCP_PHASE_CONFIRM)
    {
        return false;
    }

    bool updated = false;
    if (!mCurrentBallot)
    {
        bumpToBallot(ballot, true);
        updated = true;
    }
    else
    {
        dbgAssert(compareBallots(*mCurrentBallot, ballot) <= 0);

        if (mCommit && !areBallotsCompatible(*mCommit, ballot))
        {
            return false;
        }

        int comp = compareBallots(*mCurrentBallot, ballot);
        if (comp < 0)
        {
            bumpToBallot(ballot, true);
            updated = true;
        }
        else if (comp > 0)
        {
            // this code probably changes with the final version
            // of the conciliator

            // this case may happen if the other nodes are not
            // following the protocol (and we end up with a smaller value)
            // not sure what is the best way to deal
            // with this situation
            CLOG(ERROR, "SCP")
                << "BallotProtocol::updateCurrentValue attempt to bump to "
                   "a smaller value";
            // can't just bump to the value as we may already have
            // statements at counter+1
            return false;
        }
    }

    if (updated)
    {
        CLOG(TRACE, "SCP") << "BallotProtocol::updateCurrentValue updated";
    }

    checkInvariants();

    return updated;
}

void
BallotProtocol::bumpToBallot(SCPBallot const& ballot, bool check)
{
    if (Logging::logDebug("SCP"))
        CLOG(DEBUG, "SCP") << "BallotProtocol::bumpToBallot"
                           << " i: " << mSlot.getSlotIndex()
                           << " b: " << mSlot.getSCP().ballotToStr(ballot);

    // `bumpToBallot` should be never called once we committed.
    dbgAssert(mPhase != SCP_PHASE_EXTERNALIZE);

    if (check)
    {
        // We should move mCurrentBallot monotonically only
        dbgAssert(!mCurrentBallot ||
                  compareBallots(ballot, *mCurrentBallot) >= 0);
    }

    bool gotBumped = !mCurrentBallot || !(*mCurrentBallot == ballot);

    mCurrentBallot = make_unique<SCPBallot>(ballot);

    mHeardFromQuorum = false;

    if (gotBumped)
        startBallotProtocolTimer();
}

void
BallotProtocol::startBallotProtocolTimer()
{
    std::chrono::milliseconds timeout =
        mSlot.getSCPDriver().computeTimeout(mCurrentBallot->counter);

    std::shared_ptr<Slot> slot = mSlot.shared_from_this();
    mSlot.getSCPDriver().setupTimer(
        mSlot.getSlotIndex(), Slot::BALLOT_PROTOCOL_TIMER, timeout,
        [slot]() { slot->getBallotProtocol().ballotProtocolTimerExpired(); });
}

void
BallotProtocol::ballotProtocolTimerExpired()
{
    // don't abandon the ballot until we have heard from a slice
    if (mHeardFromQuorum)
    {
        abandonBallot(0);
    }
    else
    {
        CLOG(DEBUG, "SCP") << "Waiting to hear from a slice.";
        startBallotProtocolTimer();
    }
}

SCPStatement
BallotProtocol::createStatement(SCPStatementType const& type)
{
    SCPStatement statement;

    checkInvariants();

    statement.pledges.type(type);
    switch (type)
    {
    case SCPStatementType::SCP_ST_PREPARE:
    {
        auto& p = statement.pledges.prepare();
        p.quorumSetHash = getLocalNode()->getQuorumSetHash();
        if (mCurrentBallot)
        {
            p.ballot = *mCurrentBallot;
        }
        if (mCommit)
        {
            p.nC = mCommit->counter;
        }
        if (mPrepared)
        {
            p.prepared.activate() = *mPrepared;
        }
        if (mPreparedPrime)
        {
            p.preparedPrime.activate() = *mPreparedPrime;
        }
        if (mHighBallot)
        {
            p.nH = mHighBallot->counter;
        }
    }
    break;
    case SCPStatementType::SCP_ST_CONFIRM:
    {
        auto& c = statement.pledges.confirm();
        c.quorumSetHash = getLocalNode()->getQuorumSetHash();
        dbgAssert(areBallotsLessAndCompatible(*mCommit, *mHighBallot));
        c.ballot = *mCurrentBallot;
        c.nPrepared = mPrepared->counter;
        c.nCommit = mCommit->counter;
        c.nH = mHighBallot->counter;
    }
    break;
    case SCPStatementType::SCP_ST_EXTERNALIZE:
    {
        dbgAssert(areBallotsLessAndCompatible(*mCommit, *mHighBallot));
        auto& e = statement.pledges.externalize();
        e.commit = *mCommit;
        e.nH = mHighBallot->counter;
        e.commitQuorumSetHash = getLocalNode()->getQuorumSetHash();
    }
    break;
    default:
        dbgAbort();
    }

    return statement;
}

void
BallotProtocol::emitCurrentStateStatement()
{
    SCPStatementType t;

    switch (mPhase)
    {
    case SCP_PHASE_PREPARE:
        t = SCP_ST_PREPARE;
        break;
    case SCP_PHASE_CONFIRM:
        t = SCP_ST_CONFIRM;
        break;
    case SCP_PHASE_EXTERNALIZE:
        t = SCP_ST_EXTERNALIZE;
        break;
    default:
        dbgAbort();
    }

    SCPStatement statement = createStatement(t);
    SCPEnvelope envelope = mSlot.createEnvelope(statement);

    bool canEmit = (mCurrentBallot != nullptr);

    // if we generate the same envelope, don't process it again
    // this can occur when updating h in PREPARE phase
    // as statements only keep track of h.n (but h.x could be different)
    auto lastEnv = mLatestEnvelopes.find(mSlot.getSCP().getLocalNodeID());

    if (lastEnv == mLatestEnvelopes.end() || !(lastEnv->second == envelope))
    {
        if (mSlot.processEnvelope(envelope, true) == SCP::EnvelopeState::VALID)
        {
            if (canEmit &&
                (!mLastEnvelope || isNewerStatement(mLastEnvelope->statement,
                                                    envelope.statement)))
            {
                mLastEnvelope = std::make_shared<SCPEnvelope>(envelope);
                // this will no-op if invoked from advanceSlot
                // as advanceSlot consolidates all messages sent
                sendLatestEnvelope();
            }
        }
        else
        {
            // there is a bug in the application if it queued up
            // a statement for itself that it considers invalid
            throw std::runtime_error("moved to a bad state (ballot protocol)");
        }
    }
}

void
BallotProtocol::checkInvariants()
{
    if (mCurrentBallot)
    {
        dbgAssert(mCurrentBallot->counter != 0);
    }
    if (mPrepared && mPreparedPrime)
    {
        dbgAssert(areBallotsLessAndIncompatible(*mPreparedPrime, *mPrepared));
    }
    if (mCommit)
    {
        dbgAssert(mCurrentBallot);
        dbgAssert(areBallotsLessAndCompatible(*mCommit, *mHighBallot));
        dbgAssert(areBallotsLessAndCompatible(*mHighBallot, *mCurrentBallot));
    }

    switch (mPhase)
    {
    case SCP_PHASE_PREPARE:
        break;
    case SCP_PHASE_CONFIRM:
        dbgAssert(mCommit);
        break;
    case SCP_PHASE_EXTERNALIZE:
        dbgAssert(mCommit);
        dbgAssert(mHighBallot);
        break;
    default:
        dbgAbort();
    }
}

std::set<SCPBallot>
BallotProtocol::getPrepareCandidates(SCPStatement const& hint)
{
    std::set<SCPBallot> hintBallots;

    switch (hint.pledges.type())
    {
    case SCP_ST_PREPARE:
    {
        auto const& prep = hint.pledges.prepare();
        hintBallots.insert(prep.ballot);
        if (prep.prepared)
        {
            hintBallots.insert(*prep.prepared);
        }
        if (prep.preparedPrime)
        {
            hintBallots.insert(*prep.preparedPrime);
        }
    }
    break;
    case SCP_ST_CONFIRM:
    {
        auto const& con = hint.pledges.confirm();
        hintBallots.insert(SCPBallot(con.nPrepared, con.ballot.value));
        hintBallots.insert(SCPBallot(UINT32_MAX, con.ballot.value));
    }
    break;
    case SCP_ST_EXTERNALIZE:
    {
        auto const& ext = hint.pledges.externalize();
        hintBallots.insert(SCPBallot(UINT32_MAX, ext.commit.value));
    }
    break;
    default:
        abort();
    };

    std::set<SCPBallot> candidates;

    while (!hintBallots.empty())
    {
        auto last = --hintBallots.end();
        SCPBallot topVote = *last;
        hintBallots.erase(last);

        auto const& val = topVote.value;

        // find candidates that may have been prepared
        for (auto const& e : mLatestEnvelopes)
        {
            SCPStatement const& st = e.second.statement;
            switch (st.pledges.type())
            {
            case SCP_ST_PREPARE:
            {
                auto const& prep = st.pledges.prepare();
                if (areBallotsLessAndCompatible(prep.ballot, topVote))
                {
                    candidates.insert(prep.ballot);
                }
                if (prep.prepared &&
                    areBallotsLessAndCompatible(*prep.prepared, topVote))
                {
                    candidates.insert(*prep.prepared);
                }
                if (prep.preparedPrime &&
                    areBallotsLessAndCompatible(*prep.preparedPrime, topVote))
                {
                    candidates.insert(*prep.preparedPrime);
                }
            }
            break;
            case SCP_ST_CONFIRM:
            {
                auto const& con = st.pledges.confirm();
                if (areBallotsCompatible(topVote, con.ballot))
                {
                    candidates.insert(topVote);
                    if (con.nPrepared < topVote.counter)
                    {
                        candidates.insert(SCPBallot(con.nPrepared, val));
                    }
                }
            }
            break;
            case SCP_ST_EXTERNALIZE:
            {
                auto const& ext = st.pledges.externalize();
                if (areBallotsCompatible(topVote, ext.commit))
                {
                    candidates.insert(topVote);
                }
            }
            break;
            default:
                abort();
            }
        }
    }

    return candidates;
}

void
BallotProtocol::updateCurrentIfNeeded()
{
    if (!mCurrentBallot || compareBallots(*mCurrentBallot, *mHighBallot) < 0)
    {
        bumpToBallot(*mHighBallot, true);
    }
}

bool
BallotProtocol::attemptPreparedAccept(SCPStatement const& hint)
{
    if (mPhase != SCP_PHASE_PREPARE && mPhase != SCP_PHASE_CONFIRM)
    {
        return false;
    }

    auto candidates = getPrepareCandidates(hint);

    // see if we can accept any of the candidates, starting with the highest
    for (auto cur = candidates.rbegin(); cur != candidates.rend(); cur++)
    {
        SCPBallot ballot = *cur;

        if (mPhase == SCP_PHASE_CONFIRM)
        {
            // only consider the ballot if it may help us increase
            // p (note: at this point, p ~ c)
            if (!areBallotsLessAndCompatible(*mPrepared, ballot))
            {
                continue;
            }
            dbgAssert(areBallotsCompatible(*mCommit, ballot));
        }

        // if we already prepared this ballot, don't bother checking again

        // if ballot <= p' ballot is neither a candidate for p nor p'
        if (mPreparedPrime && compareBallots(ballot, *mPreparedPrime) <= 0)
        {
            continue;
        }

        if (mPrepared)
        {
            // if ballot is already covered by p, skip
            if (areBallotsLessAndCompatible(ballot, *mPrepared))
            {
                continue;
            }
            // otherwise, there is a chance it increases p'
        }

        bool accepted = federatedAccept(
            // checks if any node is voting for this ballot
            [&ballot, this](SCPStatement const& st) {
                bool res;

                switch (st.pledges.type())
                {
                case SCP_ST_PREPARE:
                {
                    auto const& p = st.pledges.prepare();
                    res = areBallotsLessAndCompatible(ballot, p.ballot);
                }
                break;
                case SCP_ST_CONFIRM:
                {
                    auto const& c = st.pledges.confirm();
                    res = areBallotsCompatible(ballot, c.ballot);
                }
                break;
                case SCP_ST_EXTERNALIZE:
                {
                    auto const& e = st.pledges.externalize();
                    res = areBallotsCompatible(ballot, e.commit);
                }
                break;
                default:
                    res = false;
                    dbgAbort();
                }

                return res;
            },
            std::bind(&BallotProtocol::hasPreparedBallot, ballot, _1));
        if (accepted)
        {
            return setPreparedAccept(ballot);
        }
    }

    return false;
}

bool
BallotProtocol::setPreparedAccept(SCPBallot const& ballot)
{
    if (Logging::logDebug("SCP"))
        CLOG(DEBUG, "SCP") << "BallotProtocol::setPreparedAccept"
                           << " i: " << mSlot.getSlotIndex()
                           << " b: " << mSlot.getSCP().ballotToStr(ballot);

    // update our state
    bool didWork = setPrepared(ballot);

    // check if we also need to clear 'c'
    if (mCommit && mHighBallot)
    {
        if ((mPrepared &&
             areBallotsLessAndIncompatible(*mHighBallot, *mPrepared)) ||
            (mPreparedPrime &&
             areBallotsLessAndIncompatible(*mHighBallot, *mPreparedPrime)))
        {
            dbgAssert(mPhase == SCP_PHASE_PREPARE);
            mCommit.reset();
            didWork = true;
        }
    }

    if (didWork)
    {
        mSlot.getSCPDriver().acceptedBallotPrepared(mSlot.getSlotIndex(),
                                                    ballot);
        emitCurrentStateStatement();
    }

    return didWork;
}

bool
BallotProtocol::attemptPreparedConfirmed(SCPStatement const& hint)
{
    if (mPhase != SCP_PHASE_PREPARE)
    {
        return false;
    }

    // check if we could accept this ballot as prepared
    if (!mPrepared)
    {
        return false;
    }

    auto candidates = getPrepareCandidates(hint);

    // see if we can accept any of the candidates, starting with the highest
    SCPBallot newH;
    bool newHfound = false;
    auto cur = candidates.rbegin();
    for (; cur != candidates.rend(); cur++)
    {
        SCPBallot ballot = *cur;

        // only consider it if we can potentially raise h
        if (mHighBallot && compareBallots(*mHighBallot, ballot) >= 0)
        {
            break;
        }

        bool ratified = federatedRatify(
            std::bind(&BallotProtocol::hasPreparedBallot, ballot, _1));
        if (ratified)
        {
            newH = ballot;
            newHfound = true;
            break;
        }
    }

    bool res = false;

    if (newHfound)
    {
        SCPBallot newC;
        // now, look for newC (left as 0 if no update)
        // step (3) from the paper
        SCPBallot b = mCurrentBallot ? *mCurrentBallot : SCPBallot();
        if (!mCommit &&
            (!mPrepared || !areBallotsLessAndIncompatible(newH, *mPrepared)) &&
            (!mPreparedPrime ||
             !areBallotsLessAndIncompatible(newH, *mPreparedPrime)))
        {
            // continue where we left off (cur is at newH at this point)
            for (; cur != candidates.rend(); cur++)
            {
                SCPBallot ballot = *cur;
                if (compareBallots(ballot, b) < 0)
                {
                    break;
                }
                bool ratified = federatedRatify(
                    std::bind(&BallotProtocol::hasPreparedBallot, ballot, _1));
                if (ratified)
                {
                    newC = ballot;
                }
                else
                {
                    break;
                }
            }
        }
        res = setPreparedConfirmed(newC, newH);
    }
    return res;
}

bool
BallotProtocol::commitPredicate(SCPBallot const& ballot, Interval const& check,
                                SCPStatement const& st)
{
    bool res = false;
    auto const& pl = st.pledges;
    switch (pl.type())
    {
    case SCP_ST_PREPARE:
        break;
    case SCP_ST_CONFIRM:
    {
        auto const& c = pl.confirm();
        if (areBallotsCompatible(ballot, c.ballot))
        {
            res = c.nCommit <= check.first && check.second <= c.nH;
        }
    }
    break;
    case SCP_ST_EXTERNALIZE:
    {
        auto const& e = pl.externalize();
        if (areBallotsCompatible(ballot, e.commit))
        {
            res = e.commit.counter <= check.first;
        }
    }
    break;
    default:
        dbgAbort();
    }
    return res;
}

bool
BallotProtocol::setPreparedConfirmed(SCPBallot const& newC,
                                     SCPBallot const& newH)
{
    if (Logging::logDebug("SCP"))
        CLOG(DEBUG, "SCP") << "BallotProtocol::setPreparedConfirmed"
                           << " i: " << mSlot.getSlotIndex()
                           << " h: " << mSlot.getSCP().ballotToStr(newH);

    bool didWork = false;

    if (!mHighBallot || compareBallots(newH, *mHighBallot) > 0)
    {
        didWork = true;
        mHighBallot = make_unique<SCPBallot>(newH);
    }

    if (newC.counter != 0)
    {
        dbgAssert(!mCommit);
        mCommit = make_unique<SCPBallot>(newC);
        didWork = true;
    }

    if (didWork)
    {
        updateCurrentIfNeeded();

        mSlot.getSCPDriver().confirmedBallotPrepared(mSlot.getSlotIndex(),
                                                     newH);
        emitCurrentStateStatement();
    }

    return didWork;
}

void
BallotProtocol::findExtendedInterval(Interval& candidate,
                                     std::set<uint32> const& boundaries,
                                     std::function<bool(Interval const&)> pred)
{
    // iterate through interesting boundaries, starting from the top
    for (auto it = boundaries.rbegin(); it != boundaries.rend(); it++)
    {
        uint32 b = *it;

        Interval cur;
        if (candidate.first == 0)
        {
            // first, find the high bound
            cur = Interval(b, b);
        }
        else if (b > candidate.second) // invalid
        {
            continue;
        }
        else
        {
            cur.first = b;
            cur.second = candidate.second;
        }

        if (pred(cur))
        {
            candidate = cur;
        }
        else if (candidate.first != 0)
        {
            // could not extend further
            break;
        }
    }
}

std::set<uint32>
BallotProtocol::getCommitBoundariesFromStatements(SCPBallot const& ballot)
{
    std::set<uint32> res;
    for (auto const& env : mLatestEnvelopes)
    {
        auto const& pl = env.second.statement.pledges;
        switch (pl.type())
        {
        case SCP_ST_PREPARE:
        {
            auto const& p = pl.prepare();
            if (areBallotsCompatible(ballot, p.ballot))
            {
                if (p.nC)
                {
                    res.emplace(p.nC);
                    res.emplace(p.nH);
                }
            }
        }
        break;
        case SCP_ST_CONFIRM:
        {
            auto const& c = pl.confirm();
            if (areBallotsCompatible(ballot, c.ballot))
            {
                res.emplace(c.nCommit);
                res.emplace(c.nH);
            }
        }
        break;
        case SCP_ST_EXTERNALIZE:
        {
            auto const& e = pl.externalize();
            if (areBallotsCompatible(ballot, e.commit))
            {
                res.emplace(e.commit.counter);
                res.emplace(e.nH);
                res.emplace(UINT32_MAX);
            }
        }
        break;
        default:
            dbgAbort();
        }
    }
    return res;
}

bool
BallotProtocol::attemptAcceptCommit(SCPStatement const& hint)
{
    if (mPhase != SCP_PHASE_PREPARE && mPhase != SCP_PHASE_CONFIRM)
    {
        return false;
    }

    // extracts value from hint
    // note: ballot.counter is only used for logging purpose as we're looking at
    // possible value to commit
    SCPBallot ballot;
    switch (hint.pledges.type())
    {
    case SCPStatementType::SCP_ST_PREPARE:
    {
        auto const& prep = hint.pledges.prepare();
        if (prep.nC != 0)
        {
            ballot = SCPBallot(prep.nH, prep.ballot.value);
        }
        else
        {
            return false;
        }
    }
    break;
    case SCPStatementType::SCP_ST_CONFIRM:
    {
        auto const& con = hint.pledges.confirm();
        ballot = SCPBallot(con.nH, con.ballot.value);
    }
    break;
    case SCPStatementType::SCP_ST_EXTERNALIZE:
    {
        auto const& ext = hint.pledges.externalize();
        ballot = SCPBallot(ext.nH, ext.commit.value);
        break;
    }
    default:
        abort();
    };

    if (mPhase == SCP_PHASE_CONFIRM)
    {
        if (!areBallotsCompatible(ballot, *mHighBallot))
        {
            return false;
        }
    }

    auto pred = [&ballot, this](Interval const& cur) -> bool {
        return federatedAccept(
            [&](SCPStatement const& st) -> bool {
                bool res = false;
                auto const& pl = st.pledges;
                switch (pl.type())
                {
                case SCP_ST_PREPARE:
                {
                    auto const& p = pl.prepare();
                    if (areBallotsCompatible(ballot, p.ballot))
                    {
                        if (p.nC != 0)
                        {
                            res = p.nC <= cur.first && cur.second <= p.nH;
                        }
                    }
                }
                break;
                case SCP_ST_CONFIRM:
                {
                    auto const& c = pl.confirm();
                    if (areBallotsCompatible(ballot, c.ballot))
                    {
                        res = c.nCommit <= cur.first;
                    }
                }
                break;
                case SCP_ST_EXTERNALIZE:
                {
                    auto const& e = pl.externalize();
                    if (areBallotsCompatible(ballot, e.commit))
                    {
                        res = e.commit.counter <= cur.first;
                    }
                }
                break;
                default:
                    dbgAbort();
                }
                return res;
            },
            std::bind(&BallotProtocol::commitPredicate, ballot, cur, _1));
    };

    // build the boundaries to scan
    std::set<uint32> boundaries = getCommitBoundariesFromStatements(ballot);

    if (boundaries.empty())
    {
        return false;
    }

    // now, look for the high interval
    Interval candidate;

    findExtendedInterval(candidate, boundaries, pred);

    bool res = false;

    if (candidate.first != 0)
    {
        if (mPhase != SCP_PHASE_CONFIRM ||
            candidate.second > mHighBallot->counter)
        {
            SCPBallot c = SCPBallot(candidate.first, ballot.value);
            SCPBallot h = SCPBallot(candidate.second, ballot.value);
            res = setAcceptCommit(c, h);
        }
    }

    return res;
}

bool
BallotProtocol::setAcceptCommit(SCPBallot const& c, SCPBallot const& h)
{
    if (Logging::logDebug("SCP"))
        CLOG(DEBUG, "SCP") << "BallotProtocol::setAcceptCommit"
                           << " i: " << mSlot.getSlotIndex()
                           << " new c: " << mSlot.getSCP().ballotToStr(c)
                           << " new h: " << mSlot.getSCP().ballotToStr(h);

    bool didWork = false;

    if (!mHighBallot || !mCommit || compareBallots(*mHighBallot, h) != 0 ||
        compareBallots(*mCommit, c) != 0)
    {
        mCommit = make_unique<SCPBallot>(c);
        mHighBallot = make_unique<SCPBallot>(h);

        didWork = true;
    }

    if (mPhase == SCP_PHASE_PREPARE)
    {
        mPhase = SCP_PHASE_CONFIRM;
        if (mCurrentBallot && !areBallotsLessAndCompatible(h, *mCurrentBallot))
        {
            bumpToBallot(h, false);
        }
        mPreparedPrime.reset();

        didWork = true;
    }

    if (didWork)
    {
        updateCurrentIfNeeded();

        mSlot.getSCPDriver().acceptedCommit(mSlot.getSlotIndex(), h);
        emitCurrentStateStatement();
    }

    return didWork;
}

bool
BallotProtocol::attemptBump()
{
    if (mPhase == SCP_PHASE_PREPARE || mPhase == SCP_PHASE_CONFIRM)
    {
        // find all counters
        std::set<uint32> allCounters;
        for (auto const& e : mLatestEnvelopes)
        {
            auto const& st = e.second.statement;
            switch (st.pledges.type())
            {
            case SCP_ST_PREPARE:
            {
                auto const& p = st.pledges.prepare();
                allCounters.insert(p.ballot.counter);
            }
            break;
            case SCP_ST_CONFIRM:
            {
                auto const& c = st.pledges.confirm();
                allCounters.insert(c.ballot.counter);
            }
            break;
            case SCP_ST_EXTERNALIZE:
            {
                allCounters.insert(UINT32_MAX);
            }
            break;
            default:
                abort();
            };
        }
        uint32 targetCounter = mCurrentBallot ? mCurrentBallot->counter : 0;

        // uses 0 as a way to track if a v-blocking set is at a higher counter
        // if so, we move to that smallest counter
        allCounters.insert(targetCounter);

        // go through the counters, find the smallest not v-blocking
        for (auto it = allCounters.begin(); it != allCounters.end(); it++)
        {
            uint32 n = *it;
            if (n < targetCounter)
            {
                break;
            }

            bool vBlocking = LocalNode::isVBlocking(
                getLocalNode()->getQuorumSet(), mLatestEnvelopes,
                [&](SCPStatement const& st) {
                    bool res;
                    auto const& pl = st.pledges;
                    if (pl.type() == SCP_ST_PREPARE)
                    {
                        auto const& p = pl.prepare();
                        res = n < p.ballot.counter;
                    }
                    else
                    {
                        if (pl.type() == SCP_ST_CONFIRM)
                        {
                            res = n < pl.confirm().ballot.counter;
                        }
                        else
                        {
                            res = n != UINT32_MAX;
                        }
                    }
                    return res;
                });

            if (n == targetCounter)
            {
                // if current counter is not behind, don't do anything
                if (!vBlocking)
                {
                    break;
                }
            }
            else
            {
                if (!vBlocking)
                {
                    // move to n
                    return abandonBallot(n);
                }
            }
        }
    }

    return false;
}

bool
BallotProtocol::attemptConfirmCommit(SCPStatement const& hint)
{
    if (mPhase != SCP_PHASE_CONFIRM)
    {
        return false;
    }

    if (!mHighBallot || !mCommit)
    {
        return false;
    }

    // extracts value from hint
    // note: ballot.counter is only used for logging purpose
    SCPBallot ballot;
    switch (hint.pledges.type())
    {
    case SCPStatementType::SCP_ST_PREPARE:
    {
        return false;
    }
    break;
    case SCPStatementType::SCP_ST_CONFIRM:
    {
        auto const& con = hint.pledges.confirm();
        ballot = SCPBallot(con.nH, con.ballot.value);
    }
    break;
    case SCPStatementType::SCP_ST_EXTERNALIZE:
    {
        auto const& ext = hint.pledges.externalize();
        ballot = SCPBallot(ext.nH, ext.commit.value);
        break;
    }
    default:
        abort();
    };

    if (!areBallotsCompatible(ballot, *mCommit))
    {
        return false;
    }

    std::set<uint32> boundaries = getCommitBoundariesFromStatements(ballot);
    Interval candidate;

    auto pred = [&ballot, this](Interval const& cur) -> bool {
        return federatedRatify(
            std::bind(&BallotProtocol::commitPredicate, ballot, cur, _1));
    };

    findExtendedInterval(candidate, boundaries, pred);

    bool res = candidate.first != 0;
    if (res)
    {
        SCPBallot c = SCPBallot(candidate.first, ballot.value);
        SCPBallot h = SCPBallot(candidate.second, ballot.value);
        return setConfirmCommit(c, h);
    }
    return res;
}

bool
BallotProtocol::setConfirmCommit(SCPBallot const& c, SCPBallot const& h)
{
    if (Logging::logDebug("SCP"))
        CLOG(DEBUG, "SCP") << "BallotProtocol::setConfirmCommit"
                           << " i: " << mSlot.getSlotIndex()
                           << " new c: " << mSlot.getSCP().ballotToStr(c)
                           << " new h: " << mSlot.getSCP().ballotToStr(h);

    mCommit = make_unique<SCPBallot>(c);
    mHighBallot = make_unique<SCPBallot>(h);
    updateCurrentIfNeeded();

    mPhase = SCP_PHASE_EXTERNALIZE;

    emitCurrentStateStatement();

    mSlot.stopNomination();

    mSlot.getSCPDriver().valueExternalized(mSlot.getSlotIndex(),
                                           mCommit->value);

    return true;
}

bool
BallotProtocol::hasPreparedBallot(SCPBallot const& ballot,
                                  SCPStatement const& st)
{
    bool res;

    switch (st.pledges.type())
    {
    case SCP_ST_PREPARE:
    {
        auto const& p = st.pledges.prepare();
        res =
            (p.prepared && areBallotsLessAndCompatible(ballot, *p.prepared)) ||
            (p.preparedPrime &&
             areBallotsLessAndCompatible(ballot, *p.preparedPrime));
    }
    break;
    case SCP_ST_CONFIRM:
    {
        auto const& c = st.pledges.confirm();
        SCPBallot prepared(c.nPrepared, c.ballot.value);
        res = areBallotsLessAndCompatible(ballot, prepared);
    }
    break;
    case SCP_ST_EXTERNALIZE:
    {
        auto const& e = st.pledges.externalize();
        res = areBallotsCompatible(ballot, e.commit);
    }
    break;
    default:
        res = false;
        dbgAbort();
    }

    return res;
}

Hash
BallotProtocol::getCompanionQuorumSetHashFromStatement(SCPStatement const& st)
{
    Hash h;
    switch (st.pledges.type())
    {
    case SCP_ST_PREPARE:
        h = st.pledges.prepare().quorumSetHash;
        break;
    case SCP_ST_CONFIRM:
        h = st.pledges.confirm().quorumSetHash;
        break;
    case SCP_ST_EXTERNALIZE:
        h = st.pledges.externalize().commitQuorumSetHash;
        break;
    default:
        dbgAbort();
    }
    return h;
}

SCPBallot
BallotProtocol::getWorkingBallot(SCPStatement const& st)
{
    SCPBallot res;
    switch (st.pledges.type())
    {
    case SCP_ST_PREPARE:
        res = st.pledges.prepare().ballot;
        break;
    case SCP_ST_CONFIRM:
    {
        auto const& con = st.pledges.confirm();
        res = SCPBallot(con.nCommit, con.ballot.value);
    }
    break;
    case SCP_ST_EXTERNALIZE:
        res = st.pledges.externalize().commit;
        break;
    default:
        dbgAbort();
    }
    return res;
}

bool
BallotProtocol::setPrepared(SCPBallot const& ballot)
{
    bool didWork = false;

    if (mPrepared)
    {
        int comp = compareBallots(*mPrepared, ballot);
        if (comp < 0)
        {
            if (!areBallotsCompatible(*mPrepared, ballot))
            {
                mPreparedPrime = make_unique<SCPBallot>(*mPrepared);
            }
            mPrepared = make_unique<SCPBallot>(ballot);
            didWork = true;
        }
        else if (comp > 0)
        {
            // check if we should update only p'
            if (!mPreparedPrime || compareBallots(*mPreparedPrime, ballot) < 0)
            {
                mPreparedPrime = make_unique<SCPBallot>(ballot);
                didWork = true;
            }
        }
    }
    else
    {
        mPrepared = make_unique<SCPBallot>(ballot);
        didWork = true;
    }
    return didWork;
}

int
BallotProtocol::compareBallots(std::unique_ptr<SCPBallot> const& b1,
                               std::unique_ptr<SCPBallot> const& b2)
{
    int res;
    if (b1 && b2)
    {
        res = compareBallots(*b1, *b2);
    }
    else if (b1 && !b2)
    {
        res = 1;
    }
    else if (!b1 && b2)
    {
        res = -1;
    }
    else
    {
        res = 0;
    }
    return res;
}

int
BallotProtocol::compareBallots(SCPBallot const& b1, SCPBallot const& b2)
{
    if (b1.counter < b2.counter)
    {
        return -1;
    }
    else if (b2.counter < b1.counter)
    {
        return 1;
    }
    // ballots are also strictly ordered by value
    if (b1.value < b2.value)
    {
        return -1;
    }
    else if (b2.value < b1.value)
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

bool
BallotProtocol::areBallotsCompatible(SCPBallot const& b1, SCPBallot const& b2)
{
    return b1.value == b2.value;
}

bool
BallotProtocol::areBallotsLessAndIncompatible(SCPBallot const& b1,
                                              SCPBallot const& b2)
{
    return (compareBallots(b1, b2) <= 0) && !areBallotsCompatible(b1, b2);
}

bool
BallotProtocol::areBallotsLessAndCompatible(SCPBallot const& b1,
                                            SCPBallot const& b2)
{
    return (compareBallots(b1, b2) <= 0) && areBallotsCompatible(b1, b2);
}

void
BallotProtocol::setStateFromEnvelope(SCPEnvelope const& e)
{
    if (mCurrentBallot)
    {
        throw std::runtime_error(
            "Cannot set state after starting ballot protocol");
    }

    recordEnvelope(e);

    mLastEnvelope = std::make_shared<SCPEnvelope>(e);
    mLastEnvelopeEmit = mLastEnvelope;

    auto const& pl = e.statement.pledges;

    switch (pl.type())
    {
    case SCPStatementType::SCP_ST_PREPARE:
    {
        auto const& prep = pl.prepare();
        auto const& b = prep.ballot;
        bumpToBallot(b, true);
        if (prep.prepared)
        {
            mPrepared = make_unique<SCPBallot>(*prep.prepared);
        }
        if (prep.preparedPrime)
        {
            mPreparedPrime = make_unique<SCPBallot>(*prep.preparedPrime);
        }
        if (prep.nH)
        {
            mHighBallot = make_unique<SCPBallot>(prep.nH, b.value);
        }
        if (prep.nC)
        {
            mCommit = make_unique<SCPBallot>(prep.nC, b.value);
        }
        mPhase = SCP_PHASE_PREPARE;
    }
    break;
    case SCPStatementType::SCP_ST_CONFIRM:
    {
        auto const& c = pl.confirm();
        auto const& v = c.ballot.value;
        bumpToBallot(c.ballot, true);
        mPrepared = make_unique<SCPBallot>(c.nPrepared, v);
        mHighBallot = make_unique<SCPBallot>(c.nH, v);
        mCommit = make_unique<SCPBallot>(c.nCommit, v);
        mPhase = SCP_PHASE_CONFIRM;
    }
    break;
    case SCPStatementType::SCP_ST_EXTERNALIZE:
    {
        auto const& ext = pl.externalize();
        auto const& v = ext.commit.value;
        bumpToBallot(SCPBallot(UINT32_MAX, v), true);
        mPrepared = make_unique<SCPBallot>(UINT32_MAX, v);
        mHighBallot = make_unique<SCPBallot>(ext.nH, v);
        mCommit = make_unique<SCPBallot>(ext.commit);
        mPhase = SCP_PHASE_EXTERNALIZE;
    }
    break;
    default:
        dbgAbort();
    }
}

std::vector<SCPEnvelope>
BallotProtocol::getCurrentState() const
{
    std::vector<SCPEnvelope> res;
    res.reserve(mLatestEnvelopes.size());
    for (auto const& n : mLatestEnvelopes)
    {
        // only return messages for self if the slot is fully validated
        if (!(n.first == mSlot.getSCP().getLocalNodeID()) ||
            mSlot.isFullyValidated())
        {
            res.emplace_back(n.second);
        }
    }
    return res;
}

std::vector<SCPEnvelope>
BallotProtocol::getExternalizingState() const
{
    std::vector<SCPEnvelope> res;
    if (mPhase == SCP_PHASE_EXTERNALIZE)
    {
        res.reserve(mLatestEnvelopes.size());
        for (auto const& n : mLatestEnvelopes)
        {
            if (!(n.first == mSlot.getSCP().getLocalNodeID()))
            {
                // good approximation: statements with the value that
                // externalized
                // we could filter more using mConfirmedPrepared as well
                if (areBallotsCompatible(getWorkingBallot(n.second.statement),
                                         *mCommit))
                {
                    res.emplace_back(n.second);
                }
            }
            else if (mSlot.isFullyValidated())
            {
                // only return messages for self if the slot is fully validated
                res.emplace_back(n.second);
            }
        }
    }
    return res;
}

void
BallotProtocol::advanceSlot(SCPStatement const& hint)
{
    mCurrentMessageLevel++;
    if (Logging::logDebug("SCP"))
        CLOG(DEBUG, "SCP") << "BallotProtocol::advanceSlot "
                           << mCurrentMessageLevel << " " << getLocalState();

    if (mCurrentMessageLevel >= MAX_ADVANCE_SLOT_RECURSION)
    {
        throw std::runtime_error(
            "maximum number of transitions reached in advanceSlot");
    }

    // Check if we should call `ballotDidHearFromQuorum`
    // we do this here so that we have a chance to evaluate it between
    // transitions
    // when a single message causes several
    if (!mHeardFromQuorum && mCurrentBallot)
    {
        if (LocalNode::isQuorum(
                getLocalNode()->getQuorumSet(), mLatestEnvelopes,
                std::bind(&Slot::getQuorumSetFromStatement, &mSlot, _1),
                [&](SCPStatement const& st) {
                    bool res;
                    if (st.pledges.type() == SCP_ST_PREPARE)
                    {
                        res = mCurrentBallot->counter <=
                              st.pledges.prepare().ballot.counter;
                    }
                    else
                    {
                        res = true;
                    }
                    return res;
                }))
        {
            mHeardFromQuorum = true;
            mSlot.getSCPDriver().ballotDidHearFromQuorum(mSlot.getSlotIndex(),
                                                         *mCurrentBallot);
        }
    }

    // attempt* methods will queue up messages, causing advanceSlot to be
    // called recursively

    // done in order so that we follow the steps from the white paper in
    // order
    // allowing the state to be updated properly

    bool didWork = false;

    didWork = attemptPreparedAccept(hint) || didWork;

    didWork = attemptPreparedConfirmed(hint) || didWork;

    didWork = attemptAcceptCommit(hint) || didWork;

    didWork = attemptConfirmCommit(hint) || didWork;

    // only bump after we're done with everything else
    if (mCurrentMessageLevel == 1)
    {
        bool didBump = false;
        do
        {
            // attemptBump may invoke advanceSlot recursively
            didBump = attemptBump();
            didWork = didBump || didWork;
        } while (didBump);
    }

    if (Logging::logDebug("SCP"))
        CLOG(DEBUG, "SCP") << "BallotProtocol::advanceSlot "
                           << mCurrentMessageLevel << " - exiting "
                           << getLocalState();

    --mCurrentMessageLevel;

    if (didWork)
    {
        sendLatestEnvelope();
    }
}

SCPDriver::ValidationLevel
BallotProtocol::validateValues(SCPStatement const& st)
{
    std::set<Value> values;
    switch (st.pledges.type())
    {
    case SCPStatementType::SCP_ST_PREPARE:
    {
        auto const& prep = st.pledges.prepare();
        auto const& b = prep.ballot;
        if (b.counter != 0)
        {
            values.insert(prep.ballot.value);
        }
        if (prep.prepared)
        {
            values.insert(prep.prepared->value);
        }
    }
    break;
    case SCPStatementType::SCP_ST_CONFIRM:
        values.insert(st.pledges.confirm().ballot.value);
        break;
    case SCPStatementType::SCP_ST_EXTERNALIZE:
        values.insert(st.pledges.externalize().commit.value);
        break;
    default:
        // This shouldn't happen
        return SCPDriver::kInvalidValue;
    }
    SCPDriver::ValidationLevel res = SCPDriver::kFullyValidatedValue;
    for (auto const& v : values)
    {
        auto tr =
            mSlot.getSCPDriver().validateValue(mSlot.getSlotIndex(), v, false);
        if (tr != SCPDriver::kFullyValidatedValue)
        {
            if (tr == SCPDriver::kInvalidValue)
            {
                res = SCPDriver::kInvalidValue;
            }
            else
            {
                res = SCPDriver::kMaybeValidValue;
            }
        }
    }
    return res;
}

void
BallotProtocol::sendLatestEnvelope()
{
    // emit current envelope if needed
    if (mCurrentMessageLevel == 0 && mLastEnvelope && mSlot.isFullyValidated())
    {
        if (!mLastEnvelopeEmit || mLastEnvelope != mLastEnvelopeEmit)
        {
            mLastEnvelopeEmit = mLastEnvelope;
            mSlot.getSCPDriver().emitEnvelope(*mLastEnvelopeEmit);
        }
    }
}

const char* BallotProtocol::phaseNames[SCP_PHASE_NUM] = {"PREPARE", "FINISH",
                                                         "EXTERNALIZE"};

void
BallotProtocol::dumpInfo(Json::Value& ret)
{
    Json::Value& state = ret["ballotProtocol"];
    state["heard"] = mHeardFromQuorum;
    state["ballot"] = mSlot.getSCP().ballotToStr(mCurrentBallot);
    state["phase"] = phaseNames[mPhase];

    state["state"] = getLocalState();
}

void
BallotProtocol::dumpQuorumInfo(Json::Value& ret, NodeID const& id, bool summary)
{
    auto& phase = ret["phase"];

    // find the state of the node `id`
    SCPBallot b;
    Hash qSetHash;

    auto stateit = mLatestEnvelopes.find(id);
    if (stateit == mLatestEnvelopes.end())
    {
        phase = "unknown";
        if (id == mSlot.getLocalNode()->getNodeID())
        {
            qSetHash = mSlot.getLocalNode()->getQuorumSetHash();
        }
    }
    else
    {
        auto const& st = stateit->second.statement;

        switch (st.pledges.type())
        {
        case SCPStatementType::SCP_ST_PREPARE:
            phase = "PREPARE";
            b = st.pledges.prepare().ballot;
            break;
        case SCPStatementType::SCP_ST_CONFIRM:
            phase = "CONFIRM";
            b = st.pledges.confirm().ballot;
            break;
        case SCPStatementType::SCP_ST_EXTERNALIZE:
            phase = "EXTERNALIZE";
            b = st.pledges.externalize().commit;
            break;
        default:
            dbgAbort();
        }
        // use the companion set here even for externalize to capture
        // the view of the quorum set during consensus
        qSetHash = mSlot.getCompanionQuorumSetHashFromStatement(st);
    }

    Json::Value& disagree = ret["disagree"];
    Json::Value& missing = ret["missing"];

    int n_missing = 0, n_disagree = 0;

    int agree = 0;
    auto qSet = mSlot.getSCPDriver().getQSet(qSetHash);
    if (!qSet)
    {
        phase = "expired";
        return;
    }
    LocalNode::forAllNodes(*qSet, [&](NodeID const& n) {
        auto it = mLatestEnvelopes.find(n);
        if (it == mLatestEnvelopes.end())
        {
            if (!summary)
            {
                missing.append(mSlot.getSCPDriver().toShortString(n));
            }
            n_missing++;
        }
        else if (areBallotsCompatible(getWorkingBallot(it->second.statement),
                                      b))
        {
            agree++;
        }
        else
        {
            if (!summary)
            {
                disagree.append(mSlot.getSCPDriver().toShortString(n));
            }
            n_disagree++;
        }
    });
    if (summary)
    {
        missing = n_missing;
        disagree = n_disagree;
    }

    auto f = LocalNode::findClosestVBlocking(*qSet, mLatestEnvelopes,
                                             [&](SCPStatement const& st) {
                                                 return areBallotsCompatible(
                                                     getWorkingBallot(st), b);
                                             },
                                             &id);
    ret["fail_at"] = static_cast<int>(f.size());

    if (!summary)
    {
        auto& f_ex = ret["fail_with"];
        for (auto const& n : f)
        {
            f_ex.append(mSlot.getSCPDriver().toShortString(n));
        }
        getLocalNode()->toJson(*qSet, ret["value"]);
    }

    ret["hash"] = hexAbbrev(qSetHash);
    ret["agree"] = agree;
}

std::string
BallotProtocol::getLocalState() const
{
    std::ostringstream oss;

    oss << "i: " << mSlot.getSlotIndex() << " | " << phaseNames[mPhase]
        << " | b: " << mSlot.getSCP().ballotToStr(mCurrentBallot)
        << " | p: " << mSlot.getSCP().ballotToStr(mPrepared)
        << " | p': " << mSlot.getSCP().ballotToStr(mPreparedPrime)
        << " | h: " << mSlot.getSCP().ballotToStr(mHighBallot)
        << " | c: " << mSlot.getSCP().ballotToStr(mCommit)
        << " | M: " << mLatestEnvelopes.size();
    return oss.str();
}

std::shared_ptr<LocalNode>
BallotProtocol::getLocalNode()
{
    return mSlot.getSCP().getLocalNode();
}

bool
BallotProtocol::federatedAccept(StatementPredicate voted,
                                StatementPredicate accepted)
{
    return mSlot.federatedAccept(voted, accepted, mLatestEnvelopes);
}

bool
BallotProtocol::federatedRatify(StatementPredicate voted)
{
    return mSlot.federatedRatify(voted, mLatestEnvelopes);
}
}
