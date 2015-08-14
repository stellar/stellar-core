// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "BallotProtocol.h"

#include <functional>
#include "util/types.h"
#include "xdrpp/marshal.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "util/Logging.h"
#include "scp/LocalNode.h"
#include "lib/json/json.h"
#include "util/make_unique.h"
#include "util/GlobalChecks.h"
#include "Slot.h"

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
    auto oldp = mLatestStatements.find(nodeID);
    bool res = false;

    if (oldp == mLatestStatements.end())
    {
        res = true;
    }
    else
    {
        res = isNewerStatement(oldp->second, st);
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
            // sorted by (b, p, p', P) (p' = 0 implicitely)
            auto const& oldC = oldst.pledges.confirm();
            auto const& c = st.pledges.confirm();
            if (oldC.nPrepared == c.nPrepared)
            {
                res = (oldC.nP < c.nP);
            }
            else
            {
                res = (oldC.nPrepared < c.nPrepared);
            }
        }
        else
        {
            // Lexicographical order between PREPARE statements:
            // (b, p, p', P)
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
                        res = (oldPrep.nP < prep.nP);
                    }
                }
            }
        }
    }

    return res;
}

void
BallotProtocol::recordStatement(SCPStatement const& st)
{
    auto oldp = mLatestStatements.find(st.nodeID);
    if (oldp == mLatestStatements.end())
    {
        mLatestStatements.insert(std::make_pair(st.nodeID, st));
    }
    else
    {
        oldp->second = st;
    }
    mSlot.recordStatement(st);
}

SCP::EnvelopeState
BallotProtocol::processEnvelope(SCPEnvelope const& envelope)
{
    SCP::EnvelopeState res = SCP::EnvelopeState::INVALID;
    dbgAssert(envelope.statement.slotIndex == mSlot.getSlotIndex());

    SCPStatement const& statement = envelope.statement;
    NodeID const& nodeID = statement.nodeID;

    if (!isStatementSane(statement))
    {
        return SCP::EnvelopeState::INVALID;
    }

    if (!isNewerStatement(nodeID, statement))
    {
        CLOG(TRACE, "SCP") << "stale statement, skipping "
                           << " i: " << mSlot.getSlotIndex();

        return SCP::EnvelopeState::INVALID;
    }

    SCPBallot wb = getWorkingBallot(statement);

    if (mSlot.getSCPDriver().validateValue(mSlot.getSlotIndex(), wb.value))
    {
        bool processed = false;
        SCPBallot tickBallot = getWorkingBallot(statement);

        if (mPhase != SCP_PHASE_EXTERNALIZE)
        {
            switch (statement.pledges.type())
            {
            case SCPStatementType::SCP_ST_PREPARE:
            {
                recordStatement(statement);
                processed = true;
                advanceSlot(statement.pledges.prepare().ballot);
                res = SCP::EnvelopeState::VALID;
            }
            break;
            case SCPStatementType::SCP_ST_CONFIRM:
            case SCPStatementType::SCP_ST_EXTERNALIZE:
            {
                // we don't filter CONFIRM/EXTERNALIZE statements based on
                // counter value as they are valid for any counter greater
                // than the one in the statement
                recordStatement(statement);

                // we trigger advance slot with our ballot counter if it's an
                // old statement; otherwise it's newer and may trigger progress
                // on a newer counter
                uint32 myWorkingBallotCounter = 0;
                if (mPhase == SCP_PHASE_PREPARE)
                {
                    if (mCurrentBallot)
                    {
                        myWorkingBallotCounter = mCurrentBallot->counter;
                    }
                }
                else
                {
                    myWorkingBallotCounter = mPrepared->counter;
                }
                if (tickBallot.counter < myWorkingBallotCounter)
                {
                    tickBallot.counter = myWorkingBallotCounter;
                }
                advanceSlot(tickBallot);
                res = SCP::EnvelopeState::VALID;
                processed = true;
            }
            break;
            default:
                dbgAbort();
            };
        }

        if (!processed)
        {
            // note: this handles also our own messages
            // in particular our final EXTERNALIZE message
            if (mPhase == SCP_PHASE_EXTERNALIZE &&
                mCommit->value == tickBallot.value)
            {
                recordStatement(statement);
                res = SCP::EnvelopeState::VALID;
            }
            else
            {
                res = SCP::EnvelopeState::INVALID;
            }
        }
    }
    else
    {
        // If the value is not valid, we just ignore it.
        CLOG(TRACE, "SCP") << "invalid value "
                           << " i: " << mSlot.getSlotIndex();

        res = SCP::EnvelopeState::INVALID;
    }
    return res;
}

bool
BallotProtocol::isStatementSane(SCPStatement const& st)
{
    bool res = true;

    switch (st.pledges.type())
    {
    case SCPStatementType::SCP_ST_PREPARE:
    {
        auto const& p = st.pledges.prepare();
        bool isOK = p.ballot.counter > 0;

        isOK = isOK && (!p.prepared || p.ballot.counter >= p.prepared->counter);

        isOK = isOK &&
               ((!p.preparedPrime || !p.prepared) ||
                (areBallotsLessAndIncompatible(*p.preparedPrime, *p.prepared)));

        isOK =
            isOK && (p.nP == 0 || (p.prepared && p.nP <= p.prepared->counter));

        isOK = isOK && (p.nC == 0 || (p.nP != 0 && p.nP >= p.nC));

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
        res = c.commit.counter > 0;
        res = res && c.commit.counter <= c.nP;
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
        res = res && e.nP >= e.commit.counter;

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
BallotProtocol::abandonBallot()
{
    CLOG(DEBUG, "SCP") << "BallotProtocol::abandonBallot";
    bool res = false;
    Value const& v = mSlot.getLatestCompositeCandidate();
    if(v.empty())
    {
        if(mCurrentBallot)
        {
            res = bumpState(mCurrentBallot->value, true);
        }
    } else
    {
        res = bumpState(v, true);
    }
    return res;
}

bool
BallotProtocol::bumpState(Value const& value, bool force)
{
    if (mPhase != SCP_PHASE_PREPARE)
    {
        return false;
    }

    if (!force && mCurrentBallot)
    {
        return false;
    }

    SCPBallot newb;

    if (mConfirmedPrepared)
    {
        // can only bump the counter if we committed to something already
        newb =
            SCPBallot(mCurrentBallot->counter + 1, mConfirmedPrepared->value);
    }
    else
    {
        newb.counter = mCurrentBallot ? (mCurrentBallot->counter + 1) : 1;
        newb.value = value;
    }

    CLOG(DEBUG, "SCP") << "BallotProtocol::bumpState"
                       << " i: " << mSlot.getSlotIndex()
                       << " v: " << mSlot.ballotToStr(newb);

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
    if (mPhase != SCP_PHASE_PREPARE)
    {
        return false;
    }

    bool updated = false;
    if (!mCurrentBallot)
    {
        bumpToBallot(ballot);
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
            bumpToBallot(ballot);
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
            // bumpToBallot(SCPBallot(mCurrentBallot->counter + 1,
            // ballot.value));
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
BallotProtocol::bumpToBallot(SCPBallot const& ballot)
{
    CLOG(DEBUG, "SCP") << "BallotProtocol::bumpToBallot"
        << " i: " << mSlot.getSlotIndex()
        << " b: " << mSlot.ballotToStr(ballot);

    // `bumpToBallot` should be never called once we committed.
    dbgAssert(mPhase != SCP_PHASE_EXTERNALIZE);

    // We should move mCurrentBallot monotonically only
    dbgAssert(!mCurrentBallot || compareBallots(ballot, *mCurrentBallot) >= 0);

    bool gotBumped =
        !mCurrentBallot || (mCurrentBallot->counter != ballot.counter);

    mCurrentBallot = make_unique<SCPBallot>(ballot);

    mHeardFromQuorum = false;

    if(gotBumped) startBallotProtocolTimer();
}

void 
BallotProtocol::startBallotProtocolTimer()
{
    std::chrono::milliseconds timeout =
        mSlot.getSCPDriver().computeTimeout(mCurrentBallot->counter);

   
    std::shared_ptr<Slot> slot = mSlot.shared_from_this();
    mSlot.getSCPDriver().setupTimer(
        mSlot.getSlotIndex(), Slot::BALLOT_PROTOCOL_TIMER, timeout, [slot]()
    {
        slot->getBallotProtocol().ballotProtocolTimerExpired();
    });
   
}

void 
BallotProtocol::ballotProtocolTimerExpired()
{
    // don't abandon the ballot until we have heard from a slice
    if(mHeardFromQuorum) abandonBallot();
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
        p.ballot = *mCurrentBallot;
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
        if (mConfirmedPrepared)
        {
            p.nP = mConfirmedPrepared->counter;
        }
    }
    break;
    case SCPStatementType::SCP_ST_CONFIRM:
    {
        auto& c = statement.pledges.confirm();
        c.quorumSetHash = getLocalNode()->getQuorumSetHash();
        dbgAssert(mCurrentBallot->counter == UINT32_MAX);
        dbgAssert(areBallotsLessAndCompatible(*mPrepared, *mCurrentBallot));
        dbgAssert(areBallotsLessAndCompatible(*mCommit, *mPrepared));
        dbgAssert(areBallotsLessAndCompatible(*mCommit, *mConfirmedPrepared));
        c.nPrepared = mPrepared->counter;
        c.commit = *mCommit;
        c.nPrepared = mPrepared->counter;
        c.nP = mConfirmedPrepared->counter;
    }
    break;
    case SCPStatementType::SCP_ST_EXTERNALIZE:
    {
        dbgAssert(mCurrentBallot->counter == UINT32_MAX);
        dbgAssert(areBallotsLessAndCompatible(*mPrepared, *mCurrentBallot));
        dbgAssert(areBallotsLessAndCompatible(*mCommit, *mPrepared));
        dbgAssert(areBallotsLessAndCompatible(*mCommit, *mConfirmedPrepared));
        auto& e = statement.pledges.externalize();
        e.commit = *mCommit;
        e.nP = mConfirmedPrepared->counter;
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

    if (mSlot.processEnvelope(envelope) == SCP::EnvelopeState::VALID)
    {
        if (!mLastEnvelope ||
            isNewerStatement(mLastEnvelope->statement, envelope.statement))
        {
            mLastEnvelope = make_unique<SCPEnvelope>(envelope);
            mSlot.getSCPDriver().emitEnvelope(envelope);
        }
    }
    else
    {
        // there is a bug in the application if it queued up
        // a statement for itself that it considers invalid
        throw std::runtime_error("moved to a bad state (ballot protocol)");
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
        dbgAssert(areBallotsLessAndCompatible(*mCommit, *mConfirmedPrepared));
        dbgAssert(
            areBallotsLessAndCompatible(*mConfirmedPrepared, *mCurrentBallot));
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
        dbgAssert(mConfirmedPrepared);
        break;
    default:
        dbgAbort();
    }
}

bool
BallotProtocol::attemptPrepare(SCPBallot const& ballot)
{
    bool didWork = false;
    if (mPhase == SCP_PHASE_PREPARE)
    {
        if (LocalNode::isVBlocking(
                getLocalNode()->getQuorumSet(), mLatestStatements,
                [&](SCPStatement const& st)
                {
                    bool res;
                    auto const& pl = st.pledges;
                    if (pl.type() == SCP_ST_PREPARE)
                    {
                        auto const& p = pl.prepare();
                        res = !mCurrentBallot ||
                              mCurrentBallot->counter < p.ballot.counter;
                    }
                    else
                    {
                        SCPBallot cM;
                        if (pl.type() == SCP_ST_CONFIRM)
                        {
                            cM = pl.confirm().commit;
                        }
                        else
                        {
                            cM = pl.externalize().commit;
                        }
                        res = mConfirmedPrepared &&
                              areBallotsLessAndCompatible(*mConfirmedPrepared,
                                                          cM);
                    }
                    return res;
                }))
        {
            didWork = abandonBallot();
        }
    }

    return didWork;
}

bool
BallotProtocol::isPreparedAccept(SCPBallot const& ballot)
{
    if (mPhase != SCP_PHASE_PREPARE && mPhase != SCP_PHASE_CONFIRM)
    {
        return false;
    }

    if (mPhase == SCP_PHASE_CONFIRM)
    {
        // only consider the ballot if it may help us increase
        // the Interval of prepared ballots
        if (!areBallotsLessAndCompatible(*mPrepared, ballot))
        {
            return false;
        }
        dbgAssert(areBallotsCompatible(*mCommit, ballot));
    }

    // if we already prepared this ballot, don't bother checking again
    if (mPrepared && compareBallots(ballot, *mPrepared) == 0)
    {
        return false;
    }

    return federatedAccept(
        // checks if any node is voting for this ballot
        [&ballot, this](SCPStatement const& st)
        {
            bool res;

            switch (st.pledges.type())
            {
            case SCP_ST_PREPARE:
            {
                auto const& p = st.pledges.prepare();
                res = (compareBallots(ballot, p.ballot) == 0);
            }
            break;
            case SCP_ST_CONFIRM:
            {
                auto const& c = st.pledges.confirm();
                res = areBallotsCompatible(ballot, c.commit);
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
}

bool
BallotProtocol::attemptPreparedAccept(SCPBallot const& ballot)
{
    CLOG(DEBUG, "SCP") << "BallotProtocol::attemptPreparedAccept"
                       << " i: " << mSlot.getSlotIndex()
                       << " b: " << mSlot.ballotToStr(ballot);

    // update our state
    bool didWork = false;

    // as a new ballot prepared, we also see if we can bump b
    // as there is no point in waiting to trigger the logic in attemptPrepare
    if (!mCurrentBallot)
    {
        bumpToBallot(ballot);
        didWork = true;
    }
    else if (mPhase == SCP_PHASE_PREPARE)
    {
        int comp = compareBallots(*mCurrentBallot, ballot);
        if (comp < 0)
        {
            bumpToBallot(ballot);
            didWork = true;
        }
        else if (comp > 0)
        {
            // we received an old message that allows to update p
            // after bumping b (receive some messages after timing out
            // most likely)
            CLOG(DEBUG, "SCP") << "BallotProtocol::attemptPreparedAccept "
                                  "updating p/p' after bumping b";
        }
    }

    didWork = setPrepared(ballot) || didWork;

    // check if we also need to clear 'c'
    if (mCommit && mConfirmedPrepared)
    {
        if ((mPrepared &&
             areBallotsLessAndIncompatible(*mConfirmedPrepared, *mPrepared)) ||
            (mPreparedPrime && areBallotsLessAndIncompatible(
                                   *mConfirmedPrepared, *mPreparedPrime)))
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
BallotProtocol::isPreparedConfirmed(SCPBallot const& ballot)
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

    // if we already confirmed ballot as prepared
    if (mConfirmedPrepared && compareBallots(*mConfirmedPrepared, ballot) >= 0)
    {
        return false;
    }

    return federatedRatify(
        std::bind(&BallotProtocol::hasPreparedBallot, ballot, _1));
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
        if (areBallotsCompatible(ballot, c.commit))
        {
            res = c.commit.counter <= check.first && check.second <= c.nP;
        }
    }
    break;
    case SCP_ST_EXTERNALIZE:
    {
        auto const& e = pl.externalize();
        if (areBallotsCompatible(ballot, e.commit))
        {
            res = e.commit.counter <= check.first && check.second <= e.nP;
        }
    }
    break;
    default:
        dbgAbort();
    }
    return res;
}

bool
BallotProtocol::attemptPreparedConfirmed(SCPBallot const& ballot)
{
    CLOG(DEBUG, "SCP") << "BallotProtocol::attemptPreparedConfirmed"
                       << " i: " << mSlot.getSlotIndex()
                       << " b: " << mSlot.ballotToStr(ballot);

    bool didWork = false;

    if (!mConfirmedPrepared || !(*mConfirmedPrepared == ballot))
    {
        didWork = true;
        mConfirmedPrepared = make_unique<SCPBallot>(ballot);
    }

    if (!mCommit)
    {
        if (compareBallots(mConfirmedPrepared, mCurrentBallot) >= 0)
        {
            if (!areBallotsLessAndIncompatible(*mConfirmedPrepared,
                                               *mPrepared) ||
                (mPreparedPrime &&
                 !areBallotsLessAndIncompatible(*mConfirmedPrepared,
                                                *mPreparedPrime)))
            {
                mCurrentBallot = make_unique<SCPBallot>(ballot);
                mCommit = make_unique<SCPBallot>(ballot);
                didWork = true;
            }
        }
    }

    if (didWork)
    {
        mSlot.getSCPDriver().confirmedBallotPrepared(mSlot.getSlotIndex(),
                                                     ballot);
        emitCurrentStateStatement();
    }

    return didWork;
}

void
BallotProtocol::findExtendedInterval(Interval& candidate,
                                     std::set<Interval> const& boundaries,
                                     std::function<bool(Interval const&)> pred)
{
    for (auto const& seg : boundaries)
    {
        if (candidate.second != 0)
        {
            // ensure that the segment is adjacent to the candidate we have so
            // far
            if (candidate.second < seg.first || candidate.first > seg.second)
            {
                break;
            }
        }

        dbgAssert(seg.first <= seg.second);

        for (int i = 0; i < 2; i++)
        {
            uint32 b = i ? seg.second : seg.first;

            // candidate Interval
            Interval cur;

            if (candidate.first != 0)
            {
                // see if we can expand the Interval
                cur.first = candidate.first;
                cur.second = b;
            }
            else
            {
                // see if we can pin the lowest boundary on [b,b]
                cur.first = cur.second = b;
            }

            bool keep = pred(cur);

            if (keep)
            {
                candidate = cur;
            }
            else
            {
                if (candidate.first != 0)
                {
                    // found the end of the Interval
                    break;
                }
                // otherwise, keep scanning for the lower bound
            }
        }
    }
}

std::set<BallotProtocol::Interval>
BallotProtocol::getCommitBoundariesFromStatements(SCPBallot const& ballot)
{
    std::set<Interval> res;
    for (auto const& stp : mLatestStatements)
    {
        auto const& pl = stp.second.pledges;
        switch (pl.type())
        {
        case SCP_ST_PREPARE:
        {
            auto const& p = pl.prepare();
            if (areBallotsCompatible(ballot, p.ballot))
            {
                if (p.nC)
                {
                    res.emplace(std::make_pair(p.nC, p.nP));
                }
            }
        }
        break;
        case SCP_ST_CONFIRM:
        {
            auto const& c = pl.confirm();
            if (areBallotsCompatible(ballot, c.commit))
            {
                res.emplace(std::make_pair(c.commit.counter, c.nP));
            }
        }
        break;
        case SCP_ST_EXTERNALIZE:
        {
            auto const& e = pl.externalize();
            if (areBallotsCompatible(ballot, e.commit))
            {
                res.emplace(std::make_pair(e.commit.counter, UINT32_MAX));
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
BallotProtocol::isAcceptCommit(SCPBallot const& ballot, SCPBallot& outLow,
                               SCPBallot& outHigh)
{
    if (mPhase != SCP_PHASE_PREPARE && mPhase != SCP_PHASE_CONFIRM)
    {
        return false;
    }

    if (mPhase == SCP_PHASE_CONFIRM)
    {
        if (!areBallotsCompatible(ballot, *mConfirmedPrepared))
        {
            return false;
        }
    }

    auto pred = [&ballot, this](Interval const& cur) -> bool
    {
        return federatedAccept(
            [&](SCPStatement const& st) -> bool
            {
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
                            res = p.nC <= cur.first && cur.second <= p.nP;
                        }
                    }
                }
                break;
                case SCP_ST_CONFIRM:
                {
                    auto const& c = pl.confirm();
                    if (areBallotsCompatible(ballot, c.commit))
                    {
                        res = c.commit.counter <= cur.first;
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
    std::set<Interval> boundaries = getCommitBoundariesFromStatements(ballot);

    // now see if we can accept a Interval
    Interval candidate;

    if (mPhase == SCP_PHASE_CONFIRM)
    {
        // in confirm phase we can only extend the upper bound
        candidate.first = mCommit->counter;
        candidate.second = mConfirmedPrepared->counter;

        // remove boundaries that have no chance of increasing P
        for (auto it = boundaries.begin(); it != boundaries.end();)
        {
            if (it->second <= mConfirmedPrepared->counter)
            {
                it = boundaries.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    if (boundaries.empty())
    {
        return false;
    }

    findExtendedInterval(candidate, boundaries, pred);

    bool res = false;

    if (candidate.first != 0)
    {
        if (mPhase != SCP_PHASE_CONFIRM ||
            candidate.second > mConfirmedPrepared->counter)
        {
            outLow = SCPBallot(candidate.first, ballot.value);
            outHigh = SCPBallot(candidate.second, ballot.value);
            res = true;
        }
    }

    return res;
}

bool
BallotProtocol::attemptAcceptCommit(SCPBallot const& acceptCommitLow,
                                    SCPBallot const& acceptCommitHigh)
{
    CLOG(DEBUG, "SCP") << "BallotProtocol::attemptPreparedConfirmed"
                       << " i: " << mSlot.getSlotIndex()
                       << " low: " << mSlot.ballotToStr(acceptCommitLow)
                       << " high: " << mSlot.ballotToStr(acceptCommitHigh);

    bool didWork = false;

    if (mPhase == SCP_PHASE_PREPARE ||
        areBallotsLessAndCompatible(*mConfirmedPrepared, acceptCommitHigh))
    {
        mCommit = make_unique<SCPBallot>(acceptCommitLow);
        mConfirmedPrepared = make_unique<SCPBallot>(acceptCommitHigh);
        mCurrentBallot =
            make_unique<SCPBallot>(UINT32_MAX, acceptCommitHigh.value);

        // update p if needed
        setPrepared(acceptCommitHigh);

        mPhase = SCP_PHASE_CONFIRM;
        didWork = true;
    }

    if (didWork)
    {
        mSlot.getSCPDriver().acceptedCommit(mSlot.getSlotIndex(),
                                            acceptCommitHigh);
        emitCurrentStateStatement();
    }

    return didWork;
}

bool
BallotProtocol::isConfirmCommit(SCPBallot const& ballot, SCPBallot& outLow,
                                SCPBallot& outHigh)
{
    if (mPhase != SCP_PHASE_CONFIRM)
    {
        return false;
    }

    if (!areBallotsCompatible(ballot, *mCommit))
    {
        return false;
    }

    std::set<Interval> boundaries = getCommitBoundariesFromStatements(ballot);
    Interval candidate;

    auto pred = [&ballot, this](Interval const& cur) -> bool
    {
        return federatedRatify(
            std::bind(&BallotProtocol::commitPredicate, ballot, cur, _1));
    };

    findExtendedInterval(candidate, boundaries, pred);

    bool res = candidate.first != 0;
    if (res)
    {
        outLow = SCPBallot(candidate.first, ballot.value);
        outHigh = SCPBallot(candidate.second, ballot.value);
    }
    return res;
}

bool
BallotProtocol::attemptConfirmCommit(SCPBallot const& acceptCommitLow,
                                     SCPBallot const& acceptCommitHigh)
{
    CLOG(DEBUG, "SCP") << "BallotProtocol::attemptConfirmCommit"
                       << " i: " << mSlot.getSlotIndex()
                       << " low: " << mSlot.ballotToStr(acceptCommitLow)
                       << " high: " << mSlot.ballotToStr(acceptCommitHigh);

    mCommit = make_unique<SCPBallot>(acceptCommitLow);
    mConfirmedPrepared = make_unique<SCPBallot>(acceptCommitHigh);
    mPhase = SCP_PHASE_EXTERNALIZE;

    emitCurrentStateStatement();

    mSlot.getSCPDriver().valueExternalized(mSlot.getSlotIndex(),
                                           mCurrentBallot->value);

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
        res = p.prepared && areBallotsLessAndCompatible(ballot, *p.prepared);
    }
    break;
    case SCP_ST_CONFIRM:
    {
        auto const& c = st.pledges.confirm();
        SCPBallot prepared(c.nPrepared, c.commit.value);
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
        res = SCPBallot(st.pledges.confirm().nPrepared,
                        st.pledges.confirm().commit.value);
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
        if (compareBallots(*mPrepared, ballot) < 0)
        {
            if (!areBallotsCompatible(*mPrepared, ballot))
            {
                mPreparedPrime = make_unique<SCPBallot>(*mPrepared);
            }
            mPrepared = make_unique<SCPBallot>(ballot);
            didWork = true;
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
BallotProtocol::advanceSlot(SCPBallot const& ballot)
{
    mCurrentMessageLevel++;
    CLOG(DEBUG, "SCP") << "BallotProtocol::advanceSlot " << mCurrentMessageLevel
                       << " " << getLocalState();

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
                getLocalNode()->getQuorumSet(), mLatestStatements,
                std::bind(&Slot::getQuorumSetFromStatement, &mSlot, _1),
                [&](SCPStatement const& st)
                {
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

    // 'run' is used to avoid performing work multiple times:
    // attempt* methods will queue up messages, causing advanceSlot to be
    // called recursively

    // done in order so that we follow the steps from the white paper in
    // order
    // allowing the state to be updated properly

    bool run = true;

    if (run && isPreparedAccept(ballot))
    {
        run = !attemptPreparedAccept(ballot);
    }

    if (run && isPreparedConfirmed(ballot))
    {
        run = !attemptPreparedConfirmed(ballot);
    }

    SCPBallot low, high;

    if (run && isAcceptCommit(ballot, low, high))
    {
        run = !attemptAcceptCommit(low, high);
    }

    if (run && isConfirmCommit(ballot, low, high))
    {
        run = !attemptConfirmCommit(low, high);
    }

    if (run)
    {
        // if we could not do anything, see if we should prepare
        // a different ballot
        run = attemptPrepare(ballot);
    }

    CLOG(DEBUG, "SCP") << "BallotProtocol::advanceSlot " << mCurrentMessageLevel
                       << " - exiting " << getLocalState();

    --mCurrentMessageLevel;
}

const char* BallotProtocol::phaseNames[SCP_PHASE_NUM] = {"PREPARE", "FINISH",
                                                         "EXTERNALIZE"};

void
BallotProtocol::dumpInfo(Json::Value& ret)
{
    Json::Value state;
    state["heard"] = mHeardFromQuorum;
    state["ballot"] = mSlot.ballotToStr(mCurrentBallot);
    state["phase"] = phaseNames[mPhase];

    state["state"] = getLocalState();

    ret["ballotProtocol"].append(state);
}

std::string
BallotProtocol::getLocalState() const
{
    std::ostringstream oss;

    oss << "i: " << mSlot.getSlotIndex() << " | " << phaseNames[mPhase]
        << " | b: " << mSlot.ballotToStr(mCurrentBallot)
        << " | p: " << mSlot.ballotToStr(mPrepared)
        << " | p': " << mSlot.ballotToStr(mPreparedPrime)
        << " | P: " << mSlot.ballotToStr(mConfirmedPrepared)
        << " | c: " << mSlot.ballotToStr(mCommit)
        << " | M: " << mLatestStatements.size();
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
    return mSlot.federatedAccept(voted, accepted, mLatestStatements);
}

bool
BallotProtocol::federatedRatify(StatementPredicate voted)
{
    return mSlot.federatedRatify(voted, mLatestStatements);
}
}
