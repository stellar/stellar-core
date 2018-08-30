// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "NominationProtocol.h"

#include "Slot.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "lib/json/json.h"
#include "scp/LocalNode.h"
#include "scp/QuorumSetUtils.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/XDROperators.h"
#include "xdrpp/marshal.h"
#include <functional>

namespace stellar
{
using namespace std::placeholders;

NominationProtocol::NominationProtocol(Slot& slot)
    : mSlot(slot), mRoundNumber(0), mNominationStarted(false)
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
        res = isNewerStatement(oldp->second.statement.pledges.nominate(), st);
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
    return mSlot.getSCPDriver().validateValue(mSlot.getSlotIndex(), v, true);
}

Value
NominationProtocol::extractValidValue(Value const& value)
{
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
NominationProtocol::recordEnvelope(SCPEnvelope const& env)
{
    auto const& st = env.statement;
    auto oldp = mLatestNominations.find(st.nodeID);
    if (oldp == mLatestNominations.end())
    {
        mLatestNominations.insert(std::make_pair(st.nodeID, env));
    }
    else
    {
        oldp->second = env;
    }
    mSlot.recordStatement(env.statement);
}

void
NominationProtocol::emitNomination()
{
    SCPStatement st;
    st.nodeID = mSlot.getLocalNode()->getNodeID();
    st.pledges.type(SCP_ST_NOMINATE);
    auto& nom = st.pledges.nominate();

    nom.quorumSetHash = mSlot.getLocalNode()->getQuorumSetHash();

    for (auto const& v : mVotes)
    {
        nom.votes.emplace_back(v);
    }
    for (auto const& a : mAccepted)
    {
        nom.accepted.emplace_back(a);
    }

    SCPEnvelope envelope = mSlot.createEnvelope(st);

    if (mSlot.processEnvelope(envelope, true) == SCP::EnvelopeState::VALID)
    {
        if (!mLastEnvelope ||
            isNewerStatement(mLastEnvelope->statement.pledges.nominate(),
                             st.pledges.nominate()))
        {
            mLastEnvelope = std::make_unique<SCPEnvelope>(envelope);
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
    for (auto const& a : nom.accepted)
    {
        processor(a);
    }
}

void
NominationProtocol::updateRoundLeaders()
{
    SCPQuorumSet myQSet = mSlot.getLocalNode()->getQuorumSet();

    // initialize priority with value derived from self
    mRoundLeaders.clear();
    auto localID = mSlot.getLocalNode()->getNodeID();
    normalizeQSet(myQSet, &localID);

    mRoundLeaders.insert(localID);
    uint64 topPriority = getNodePriority(localID, myQSet);

    LocalNode::forAllNodes(myQSet, [&](NodeID const& cur) {
        uint64 w = getNodePriority(cur, myQSet);
        if (w > topPriority)
        {
            topPriority = w;
            mRoundLeaders.clear();
        }
        if (w == topPriority && w > 0)
        {
            mRoundLeaders.insert(cur);
        }
    });
    CLOG(DEBUG, "SCP") << "updateRoundLeaders: " << mRoundLeaders.size();
    if (Logging::logDebug("SCP"))
        for (auto const& rl : mRoundLeaders)
        {
            CLOG(DEBUG, "SCP")
                << "    leader " << mSlot.getSCPDriver().toShortString(rl);
        }
}

uint64
NominationProtocol::hashNode(bool isPriority, NodeID const& nodeID)
{
    dbgAssert(!mPreviousValue.empty());
    return mSlot.getSCPDriver().computeHashNode(
        mSlot.getSlotIndex(), mPreviousValue, isPriority, mRoundNumber, nodeID);
}

uint64
NominationProtocol::hashValue(Value const& value)
{
    dbgAssert(!mPreviousValue.empty());
    return mSlot.getSCPDriver().computeValueHash(
        mSlot.getSlotIndex(), mPreviousValue, mRoundNumber, value);
}

uint64
NominationProtocol::getNodePriority(NodeID const& nodeID,
                                    SCPQuorumSet const& qset)
{
    uint64 res;
    uint64 w;

    if (nodeID == mSlot.getLocalNode()->getNodeID())
    {
        // local node is in all quorum sets
        w = UINT64_MAX;
    }
    else
    {
        w = LocalNode::getNodeWeight(nodeID, qset);
    }

    if (hashNode(false, nodeID) < w)
    {
        res = hashNode(true, nodeID);
    }
    else
    {
        res = 0;
    }
    return res;
}

Value
NominationProtocol::getNewValueFromNomination(SCPNomination const& nom)
{
    // pick the highest value we don't have from the leader
    // sorted using hashValue.
    Value newVote;
    uint64 newHash = 0;

    applyAll(nom, [&](Value const& value) {
        Value valueToNominate;
        auto vl = validateValue(value);
        if (vl == SCPDriver::kFullyValidatedValue)
        {
            valueToNominate = value;
        }
        else
        {
            valueToNominate = extractValidValue(value);
        }
        if (!valueToNominate.empty())
        {
            if (mVotes.find(valueToNominate) == mVotes.end())
            {
                uint64 curHash = hashValue(valueToNominate);
                if (curHash >= newHash)
                {
                    newHash = curHash;
                    newVote = valueToNominate;
                }
            }
        }
    });
    return newVote;
}

SCP::EnvelopeState
NominationProtocol::processEnvelope(SCPEnvelope const& envelope)
{
    auto const& st = envelope.statement;
    auto const& nom = st.pledges.nominate();

    SCP::EnvelopeState res = SCP::EnvelopeState::INVALID;

    if (isNewerStatement(st.nodeID, nom))
    {
        if (isSane(st))
        {
            recordEnvelope(envelope);
            res = SCP::EnvelopeState::VALID;

            if (mNominationStarted)
            {
                bool modified =
                    false; // tracks if we should emit a new nomination message
                bool newCandidates = false;

                // attempts to promote some of the votes to accepted
                for (auto const& v : nom.votes)
                {
                    if (mAccepted.find(v) != mAccepted.end())
                    { // v is already accepted
                        continue;
                    }
                    if (mSlot.federatedAccept(
                            [&v](SCPStatement const& st) -> bool {
                                auto const& nom = st.pledges.nominate();
                                bool res;
                                res = (std::find(nom.votes.begin(),
                                                 nom.votes.end(),
                                                 v) != nom.votes.end());
                                return res;
                            },
                            std::bind(&NominationProtocol::acceptPredicate, v,
                                      _1),
                            mLatestNominations))
                    {
                        auto vl = validateValue(v);
                        if (vl == SCPDriver::kFullyValidatedValue)
                        {
                            mAccepted.emplace(v);
                            mVotes.emplace(v);
                            modified = true;
                        }
                        else
                        {
                            // the value made it pretty far:
                            // see if we can vote for a variation that
                            // we consider valid
                            Value toVote;
                            toVote = extractValidValue(v);
                            if (!toVote.empty())
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
                            std::bind(&NominationProtocol::acceptPredicate, a,
                                      _1),
                            mLatestNominations))
                    {
                        mCandidates.emplace(a);
                        newCandidates = true;
                    }
                }

                // only take round leader votes if we're still looking for
                // candidates
                if (mCandidates.empty() &&
                    mRoundLeaders.find(st.nodeID) != mRoundLeaders.end())
                {
                    Value newVote = getNewValueFromNomination(nom);
                    if (!newVote.empty())
                    {
                        mVotes.emplace(newVote);
                        modified = true;
                        mSlot.getSCPDriver().nominatingValue(
                            mSlot.getSlotIndex(), newVote);
                    }
                }

                if (modified)
                {
                    emitNomination();
                }

                if (newCandidates)
                {
                    mLatestCompositeCandidate =
                        mSlot.getSCPDriver().combineCandidates(
                            mSlot.getSlotIndex(), mCandidates);

                    mSlot.getSCPDriver().updatedCandidateValue(
                        mSlot.getSlotIndex(), mLatestCompositeCandidate);

                    mSlot.bumpState(mLatestCompositeCandidate, false);
                }
            }
        }
        else
        {
            CLOG(DEBUG, "SCP")
                << "NominationProtocol: message didn't pass sanity check";
        }
    }
    return res;
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
NominationProtocol::nominate(Value const& value, Value const& previousValue,
                             bool timedout)
{
    if (Logging::logDebug("SCP"))
        CLOG(DEBUG, "SCP") << "NominationProtocol::nominate (" << mRoundNumber
                           << ") " << mSlot.getSCP().getValueString(value);

    bool updated = false;

    if (timedout && !mNominationStarted)
    {
        CLOG(DEBUG, "SCP") << "NominationProtocol::nominate (TIMED OUT)";
        return false;
    }

    mNominationStarted = true;

    mPreviousValue = previousValue;

    mRoundNumber++;
    updateRoundLeaders();

    Value nominatingValue;

    if (mRoundLeaders.find(mSlot.getLocalNode()->getNodeID()) !=
        mRoundLeaders.end())
    {
        auto ins = mVotes.insert(value);
        if (ins.second)
        {
            updated = true;
        }
        nominatingValue = value;
    }
    else
    {
        for (auto const& leader : mRoundLeaders)
        {
            auto it = mLatestNominations.find(leader);
            if (it != mLatestNominations.end())
            {
                nominatingValue = getNewValueFromNomination(
                    it->second.statement.pledges.nominate());
                if (!nominatingValue.empty())
                {
                    mVotes.insert(nominatingValue);
                    updated = true;
                }
            }
        }
    }

    std::chrono::milliseconds timeout =
        mSlot.getSCPDriver().computeTimeout(mRoundNumber);

    mSlot.getSCPDriver().nominatingValue(mSlot.getSlotIndex(), nominatingValue);

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
        CLOG(DEBUG, "SCP") << "NominationProtocol::nominate (SKIPPED)";
    }

    return updated;
}

void
NominationProtocol::stopNomination()
{
    mNominationStarted = false;
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
        ret["X"][counter] = mSlot.getSCP().getValueString(v);
        counter++;
    }

    counter = 0;
    for (auto const& v : mAccepted)
    {
        ret["Y"][counter] = mSlot.getSCP().getValueString(v);
        counter++;
    }

    counter = 0;
    for (auto const& v : mCandidates)
    {
        ret["Z"][counter] = mSlot.getSCP().getValueString(v);
        counter++;
    }

    return ret;
}

void
NominationProtocol::setStateFromEnvelope(SCPEnvelope const& e)
{
    if (mNominationStarted)
    {
        throw std::runtime_error(
            "Cannot set state after nomination is started");
    }
    recordEnvelope(e);
    auto const& nom = e.statement.pledges.nominate();
    for (auto const& a : nom.accepted)
    {
        mAccepted.emplace(a);
    }
    for (auto const& v : nom.votes)
    {
        mVotes.emplace(v);
    }

    mLastEnvelope = std::make_unique<SCPEnvelope>(e);
}

std::vector<SCPEnvelope>
NominationProtocol::getCurrentState() const
{
    std::vector<SCPEnvelope> res;
    res.reserve(mLatestNominations.size());
    for (auto const& n : mLatestNominations)
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
}
