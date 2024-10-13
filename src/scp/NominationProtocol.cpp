// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "NominationProtocol.h"

#include "Slot.h"
#include "crypto/Hex.h"
#include "lib/json/json.h"
#include "scp/LocalNode.h"
#include "scp/QuorumSetUtils.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/XDROperators.h"
#include "xdrpp/marshal.h"
#include <Tracy.hpp>
#include <algorithm>
#include <functional>

namespace stellar
{
using namespace std::placeholders;

NominationProtocol::NominationProtocol(Slot& slot)
    : mSlot(slot), mRoundNumber(0), mNominationStarted(false), mTimerExpCount(0)
{
}

bool
NominationProtocol::isNewerStatement(NodeID const& nodeID,
                                     SCPNomination const& st)
{
    auto oldp = mLatestNominations.find(nodeID);
    bool res = false;

    if (oldp == mLatestNominations.end())
    {
        res = true;
    }
    else
    {
        res = isNewerStatement(oldp->second->getStatement().pledges.nominate(),
                               st);
    }
    return res;
}

bool
NominationProtocol::isSubsetHelper(xdr::xvector<Value> const& p,
                                   xdr::xvector<Value> const& v, bool& notEqual)
{
    bool res;
    if (p.size() <= v.size())
    {
        res = std::includes(v.begin(), v.end(), p.begin(), p.end());
        if (res)
        {
            notEqual = p.size() != v.size();
        }
        else
        {
            notEqual = true;
        }
    }
    else
    {
        notEqual = true;
        res = false;
    }
    return res;
}

SCPDriver::ValidationLevel
NominationProtocol::validateValue(Value const& v)
{
    ZoneScoped;
    return mSlot.getSCPDriver().validateValue(mSlot.getSlotIndex(), v, true);
}

ValueWrapperPtr
NominationProtocol::extractValidValue(Value const& value)
{
    ZoneScoped;
    return mSlot.getSCPDriver().extractValidValue(mSlot.getSlotIndex(), value);
}

bool
NominationProtocol::isNewerStatement(SCPNomination const& oldst,
                                     SCPNomination const& st)
{
    bool res = false;
    bool grows;
    bool g = false;

    if (isSubsetHelper(oldst.votes, st.votes, g))
    {
        grows = g;
        if (isSubsetHelper(oldst.accepted, st.accepted, g))
        {
            grows = grows || g;
            res = grows; //  true only if one of the sets grew
        }
    }

    return res;
}

bool
NominationProtocol::isSane(SCPStatement const& st)
{
    auto const& nom = st.pledges.nominate();
    bool res = (nom.votes.size() + nom.accepted.size()) != 0;

    res = res && (std::adjacent_find(
                      nom.votes.begin(), nom.votes.end(),
                      [](stellar::Value const& l, stellar::Value const& r) {
                          return !(l < r);
                      }) == nom.votes.end());
    res = res && (std::adjacent_find(
                      nom.accepted.begin(), nom.accepted.end(),
                      [](stellar::Value const& l, stellar::Value const& r) {
                          return !(l < r);
                      }) == nom.accepted.end());

    return res;
}

// only called after a call to isNewerStatement so safe to replace the
// mLatestNomination
void
NominationProtocol::recordEnvelope(SCPEnvelopeWrapperPtr env)
{
    auto const& st = env->getStatement();
    auto oldp = mLatestNominations.find(st.nodeID);
    if (oldp == mLatestNominations.end())
    {
        mLatestNominations.insert(std::make_pair(st.nodeID, env));
    }
    else
    {
        oldp->second = env;
    }
    mSlot.recordStatement(env->getStatement());
}

void
NominationProtocol::emitNomination()
{
    ZoneScoped;
    SCPStatement st;
    st.nodeID = mSlot.getLocalNode()->getNodeID();
    st.pledges.type(SCP_ST_NOMINATE);
    auto& nom = st.pledges.nominate();

    nom.quorumSetHash = mSlot.getLocalNode()->getQuorumSetHash();

    for (auto const& v : mVotes)
    {
        nom.votes.emplace_back(v->getValue());
    }
    for (auto const& a : mAccepted)
    {
        nom.accepted.emplace_back(a->getValue());
    }

    SCPEnvelope envelope = mSlot.createEnvelope(st);

    auto envW = mSlot.getSCPDriver().wrapEnvelope(envelope);

    if (mSlot.processEnvelope(envW, true) == SCP::EnvelopeState::VALID)
    {
        if (!mLastEnvelope ||
            isNewerStatement(mLastEnvelope->getStatement().pledges.nominate(),
                             st.pledges.nominate()))
        {
            mLastEnvelope = envW;
            if (mSlot.isFullyValidated())
            {
                mSlot.getSCPDriver().emitEnvelope(envelope);
            }
        }
    }
    else
    {
        // there is a bug in the application if it queued up
        // a statement for itself that it considers invalid
        throw std::runtime_error("moved to a bad state (nomination)");
    }
}

bool
NominationProtocol::acceptPredicate(Value const& v, SCPStatement const& st)
{
    auto const& nom = st.pledges.nominate();
    bool res;
    res = (std::find(nom.accepted.begin(), nom.accepted.end(), v) !=
           nom.accepted.end());
    return res;
}

void
NominationProtocol::applyAll(SCPNomination const& nom,
                             std::function<void(Value const&)> processor)
{
    for (auto const& v : nom.votes)
    {
        processor(v);
    }
    // NB: "accepted" should be a subset of "votes", so this should no-op
    for (auto const& a : nom.accepted)
    {
        if (std::find(nom.votes.begin(), nom.votes.end(), a) == nom.votes.end())
        {
            processor(a);
        }
    }
}

void
NominationProtocol::updateRoundLeaders()
{
    ZoneScoped;
    SCPQuorumSet myQSet = mSlot.getLocalNode()->getQuorumSet();

    auto localID = mSlot.getLocalNode()->getNodeID();
    normalizeQSet(myQSet, &localID); // excludes self

    size_t maxLeaderCount = 1; // includes self
    // note that node IDs here are unique ("sane"), so we can count by
    // enumeration
    LocalNode::forAllNodes(myQSet, [&](NodeID const& cur) {
        ++maxLeaderCount;
        return true;
    });

    while (mRoundLeaders.size() < maxLeaderCount)
    {
        // initialize priority with value derived from self
        std::set<NodeID> newRoundLeaders;

        newRoundLeaders.insert(localID);
        uint64 topPriority = getNodePriority(localID, myQSet);

        LocalNode::forAllNodes(myQSet, [&](NodeID const& cur) {
            uint64 w = getNodePriority(cur, myQSet);
            if (w > topPriority)
            {
                topPriority = w;
                newRoundLeaders.clear();
            }
            if (w == topPriority && w > 0)
            {
                newRoundLeaders.insert(cur);
            }
            return true;
        });

        if (topPriority == 0)
        {
            // No one had priority, so all nodes would choose themselves
            // resulting in a timeout. Clear newRoundLeaders, allowing the
            // algorithm to fast timeout and try again.
            newRoundLeaders.clear();
        }

        // expand mRoundLeaders with the newly computed leaders
        auto oldSize = mRoundLeaders.size();
        mRoundLeaders.insert(newRoundLeaders.begin(), newRoundLeaders.end());
        if (oldSize != mRoundLeaders.size())
        {
            if (Logging::logDebug("SCP"))
            {
                CLOG_DEBUG(SCP, "updateRoundLeaders: {} -> {}", oldSize,
                           mRoundLeaders.size());
                for (auto const& rl : mRoundLeaders)
                {
                    CLOG_DEBUG(SCP, "    leader {}",
                               mSlot.getSCPDriver().toShortString(rl));
                }
            }
            return;
        }
        else
        {
            mRoundNumber++;
            CLOG_DEBUG(SCP,
                       "updateRoundLeaders: fast timeout (would no op) -> {}",
                       mRoundNumber);
        }
    }
    CLOG_DEBUG(SCP, "updateRoundLeaders: nothing to do");
}

uint64
NominationProtocol::hashNode(bool isPriority, NodeID const& nodeID)
{
    ZoneScoped;
    dbgAssert(!mPreviousValue.empty());
    return mSlot.getSCPDriver().computeHashNode(
        mSlot.getSlotIndex(), mPreviousValue, isPriority, mRoundNumber, nodeID);
}

uint64
NominationProtocol::hashValue(Value const& value)
{
    ZoneScoped;
    dbgAssert(!mPreviousValue.empty());
    return mSlot.getSCPDriver().computeValueHash(
        mSlot.getSlotIndex(), mPreviousValue, mRoundNumber, value);
}

uint64
NominationProtocol::getNodePriority(NodeID const& nodeID,
                                    SCPQuorumSet const& qset)
{
    ZoneScoped;
    uint64 res;
    uint64 w = mSlot.getSCPDriver().getNodeWeight(
        nodeID, qset, nodeID == mSlot.getLocalNode()->getNodeID());

    // if w > 0; w is inclusive here as
    // 0 <= hashNode <= UINT64_MAX
    if (w > 0 && hashNode(false, nodeID) <= w)
    {
        res = hashNode(true, nodeID);
    }
    else
    {
        res = 0;
    }
    return res;
}

ValueWrapperPtr
NominationProtocol::getNewValueFromNomination(SCPNomination const& nom)
{
    ZoneScoped;
    // pick the highest value we don't have from the leader
    // sorted using hashValue.
    ValueWrapperPtr newVote;
    uint64 newHash = 0;
    bool foundValidValue = false;

    auto pickValue = [&](Value const& value) {
        ValueWrapperPtr valueToNominate;
        auto vl = validateValue(value);
        if (vl == SCPDriver::kFullyValidatedValue)
        {
            valueToNominate = mSlot.getSCPDriver().wrapValue(value);
        }
        else
        {
            valueToNominate = extractValidValue(value);
        }
        if (valueToNominate)
        {
            foundValidValue = true;
            if (mVotes.find(valueToNominate) == mVotes.end())
            {
                uint64 curHash = hashValue(valueToNominate->getValue());
                if (curHash >= newHash)
                {
                    newHash = curHash;
                    newVote = valueToNominate;
                }
            }
        }
    };

    for (auto const& val : nom.accepted)
    {
        pickValue(val);
    }

    // Move on to votes if we have not found a valid accepted value
    if (!foundValidValue)
    {
        for (auto const& val : nom.votes)
        {
            pickValue(val);
        }
    }

    return newVote;
}

SCP::EnvelopeState
NominationProtocol::processEnvelope(SCPEnvelopeWrapperPtr envelope)
{
    ZoneScoped;
    auto const& st = envelope->getStatement();
    auto const& nom = st.pledges.nominate();

    if (!isNewerStatement(st.nodeID, nom))
        return SCP::EnvelopeState::INVALID;

    if (!isSane(st))
    {
        CLOG_TRACE(SCP, "NominationProtocol: message didn't pass sanity check");
        return SCP::EnvelopeState::INVALID;
    }

    recordEnvelope(envelope);

    if (mNominationStarted)
    {
        // tracks if we should emit a new nomination message
        bool modified = false;
        bool newCandidates = false;

        // attempts to promote some of the votes to accepted
        for (auto const& v : nom.votes)
        {
            auto vw = mSlot.getSCPDriver().wrapValue(v);
            if (mAccepted.find(vw) != mAccepted.end())
            { // v is already accepted
                continue;
            }
            if (mSlot.federatedAccept(
                    [&v](SCPStatement const& st) -> bool {
                        auto const& nom = st.pledges.nominate();
                        bool res;
                        res = (std::find(nom.votes.begin(), nom.votes.end(),
                                         v) != nom.votes.end());
                        return res;
                    },
                    std::bind(&NominationProtocol::acceptPredicate, v, _1),
                    mLatestNominations))
            {
                auto vl = validateValue(v);
                if (vl == SCPDriver::kFullyValidatedValue)
                {
                    mAccepted.emplace(vw);
                    mVotes.emplace(vw);
                    modified = true;
                }
                else
                {
                    // the value made it pretty far:
                    // see if we can vote for a variation that
                    // we consider valid
                    auto toVote = extractValidValue(v);
                    if (toVote)
                    {
                        if (mVotes.emplace(toVote).second)
                        {
                            modified = true;
                        }
                    }
                }
            }
        }
        // attempts to promote accepted values to candidates
        for (auto const& a : mAccepted)
        {
            if (mCandidates.find(a) != mCandidates.end())
            {
                continue;
            }
            if (mSlot.federatedRatify(
                    std::bind(&NominationProtocol::acceptPredicate,
                              a->getValue(), _1),
                    mLatestNominations))
            {
                mCandidates.emplace(a);
                newCandidates = true;
                // Stop the timer, as there's no need to continue nominating,
                // per the whitepaper:
                // "As soon as `v` has a candidate value, however, it must cease
                // voting to nominate `x` for any new values `x`"
                mSlot.getSCPDriver().stopTimer(mSlot.getSlotIndex(),
                                               Slot::NOMINATION_TIMER);
            }
        }

        // only take round leader votes if we're still looking for
        // candidates
        if (mCandidates.empty() &&
            mRoundLeaders.find(st.nodeID) != mRoundLeaders.end())
        {
            auto newVote = getNewValueFromNomination(nom);
            if (newVote)
            {
                mVotes.emplace(newVote);
                modified = true;
                mSlot.getSCPDriver().nominatingValue(mSlot.getSlotIndex(),
                                                     newVote->getValue());
            }
        }

        if (modified)
        {
            emitNomination();
        }

        if (newCandidates)
        {
            mLatestCompositeCandidate = mSlot.getSCPDriver().combineCandidates(
                mSlot.getSlotIndex(), mCandidates);

            mSlot.getSCPDriver().updatedCandidateValue(
                mSlot.getSlotIndex(), mLatestCompositeCandidate->getValue());

            mSlot.bumpState(mLatestCompositeCandidate->getValue(), false);
        }
    }
    return SCP::EnvelopeState::VALID;
}

std::vector<Value>
NominationProtocol::getStatementValues(SCPStatement const& st)
{
    std::vector<Value> res;
    applyAll(st.pledges.nominate(),
             [&](Value const& v) { res.emplace_back(v); });
    return res;
}

// attempts to nominate a value for consensus
bool
NominationProtocol::nominate(ValueWrapperPtr value, Value const& previousValue,
                             bool timedout)
{
    ZoneScoped;

    // No need to continue nominating, as per the whitepaper:
    // "As soon as `v` has a candidate value, however, it must cease
    // voting to nominate `x` for any new values `x`"
    if (!mCandidates.empty())
    {
        CLOG_DEBUG(SCP, "Skip nomination round {}, already have a candidate",
                   mRoundNumber);
        return false;
    }

    CLOG_DEBUG(SCP, "NominationProtocol::nominate ({}) {}", mRoundNumber,
               mSlot.getSCP().getValueString(value->getValue()));

    bool updated = false;

    if (timedout)
    {
        mTimerExpCount++;
    }

    if (timedout && !mNominationStarted)
    {
        CLOG_DEBUG(SCP, "NominationProtocol::nominate (TIMED OUT)");
        return false;
    }

    mNominationStarted = true;

    mPreviousValue = previousValue;

    mRoundNumber++;
    updateRoundLeaders();

    std::chrono::milliseconds timeout =
        mSlot.getSCPDriver().computeTimeout(mRoundNumber);

    // add a few more values from other leaders
    for (auto const& leader : mRoundLeaders)
    {
        auto it = mLatestNominations.find(leader);
        if (it != mLatestNominations.end())
        {
            auto lnmV = getNewValueFromNomination(
                it->second->getStatement().pledges.nominate());
            if (lnmV)
            {
                mVotes.insert(lnmV);
                updated = true;
                mSlot.getSCPDriver().nominatingValue(mSlot.getSlotIndex(),
                                                     lnmV->getValue());
            }
        }
    }

    // if we're leader, add our value if we haven't added any votes yet
    if (mRoundLeaders.find(mSlot.getLocalNode()->getNodeID()) !=
            mRoundLeaders.end() &&
        mVotes.empty())
    {
        auto ins = mVotes.insert(value);
        if (ins.second)
        {
            updated = true;
            mSlot.getSCPDriver().nominatingValue(mSlot.getSlotIndex(),
                                                 value->getValue());
        }
    }

    std::shared_ptr<Slot> slot = mSlot.shared_from_this();
    mSlot.getSCPDriver().setupTimer(
        mSlot.getSlotIndex(), Slot::NOMINATION_TIMER, timeout,
        [slot, value, previousValue]() {
            slot->nominate(value, previousValue, true);
        });

    if (updated)
    {
        emitNomination();
    }
    else
    {
        CLOG_DEBUG(SCP, "NominationProtocol::nominate (SKIPPED)");
    }

    return updated;
}

void
NominationProtocol::stopNomination()
{
    mNominationStarted = false;
}

std::set<NodeID> const&
NominationProtocol::getLeaders() const
{
    return mRoundLeaders;
}

Json::Value
NominationProtocol::getJsonInfo()
{
    Json::Value ret;
    ret["roundnumber"] = mRoundNumber;
    ret["started"] = mNominationStarted;

    int counter = 0;
    for (auto const& v : mVotes)
    {
        ret["X"][counter] = mSlot.getSCP().getValueString(v->getValue());
        counter++;
    }

    counter = 0;
    for (auto const& v : mAccepted)
    {
        ret["Y"][counter] = mSlot.getSCP().getValueString(v->getValue());
        counter++;
    }

    counter = 0;
    for (auto const& v : mCandidates)
    {
        ret["Z"][counter] = mSlot.getSCP().getValueString(v->getValue());
        counter++;
    }

    return ret;
}

SCP::QuorumInfoNodeState
NominationProtocol::getState(NodeID const& n, bool selfAlreadyMovedOn)
{
    auto state = SCP::QuorumInfoNodeState::AGREE;
    if (n == mSlot.getLocalNode()->getNodeID())
    {
        // always mark myself as AGREE.
        return state;
    }

    bool enoughTimeHasPassed =
        mTimerExpCount >= Slot::NUM_TIMEOUTS_THRESHOLD_FOR_REPORTING ||

        selfAlreadyMovedOn;

    auto it = mLatestNominations.find(n);
    if (it == mLatestNominations.end())
    {
        if (enoughTimeHasPassed)
        {
            state = SCP::QuorumInfoNodeState::MISSING;
        }
        else
        {
            // Not enough time has passed to start calling this node missing.
            state = SCP::QuorumInfoNodeState::NO_INFO;
        }
    }
    else
    {
        if (enoughTimeHasPassed && mLastEnvelope)
        {
            // Enough time has passed & we have some information on this node.
            // We'll examine the information to determine the state of this
            // node.
            auto const& other =
                it->second->getStatement().pledges.nominate().accepted;
            auto const& mine =
                mLastEnvelope->getStatement().pledges.nominate().accepted;

            bool notEq, isSubset = isSubsetHelper(other, mine, notEq);
            if (isSubset)
            {
                if (notEq)
                {
                    // If there's some values that I've accepted that
                    // they haven't, they are slow.
                    state = SCP::QuorumInfoNodeState::DELAYED;
                }
            }
            else
            {
                // If they have accepted a value that I haven't,
                // it's likely that I'll never accept it.
                state = SCP::QuorumInfoNodeState::DISAGREE;
            }
        }
    }
    return state;
}

void
NominationProtocol::setStateFromEnvelope(SCPEnvelopeWrapperPtr e)
{
    if (mNominationStarted)
    {
        throw std::runtime_error(
            "Cannot set state after nomination is started");
    }
    recordEnvelope(e);
    auto const& nom = e->getStatement().pledges.nominate();
    for (auto const& a : nom.accepted)
    {
        mAccepted.emplace(mSlot.getSCPDriver().wrapValue(a));
    }
    for (auto const& v : nom.votes)
    {
        mVotes.emplace(mSlot.getSCPDriver().wrapValue(v));
    }

    mLastEnvelope = e;
}

bool
NominationProtocol::processCurrentState(
    std::function<bool(SCPEnvelope const&)> const& f, bool forceSelf) const
{
    for (auto const& n : mLatestNominations)
    {
        // only return messages for self if the slot is fully validated
        if (forceSelf || !(n.first == mSlot.getSCP().getLocalNodeID()) ||
            mSlot.isFullyValidated())
        {
            if (!f(n.second->getEnvelope()))
            {
                return false;
            }
        }
    }
    return true;
}

SCPEnvelope const*
NominationProtocol::getLatestMessage(NodeID const& id) const
{
    auto it = mLatestNominations.find(id);
    if (it != mLatestNominations.end())
    {
        return &it->second->getEnvelope();
    }
    return nullptr;
}
}
