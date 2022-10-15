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
#include <Tracy.hpp>
#include <algorithm>
#include <functional>
// Access to loaded configs
#include "main/Config.h"

namespace stellar
{
using namespace std::placeholders;

NominationProtocol::NominationProtocol(Slot& slot)
    : mSlot(slot), mRoundNumber(0), mNominationStarted(false)
{
    CLOG_DEBUG(SCP,
               "### new NominationProtocol object, value of external global "
               "variable baseNominationTimeOutInMilisecs ({}).",
               BASE_LEADER_NOMINATION_TIMEOUT_MS);
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

/*
Execute logic to derive the leader(s) to nominate.  Algorithm (derived from
https://www.scs.stanford.edu/~dm/blog/simplified-scp.html) is:

*Note: local node is identified as "node u" while its peer defined in node u's
qset is identified as "node v".

- Compute weight for each peer (node v), include self (node u).
Node u weight is always assigned max value.
Node v  weight is a fraction of node u's quorum slice containing node v.  In
other words, weight is influence on whether local node dynamically created
slices contains node v.

NOTE: we no longer give node u an advantage in being elected.  We assigned max
value to node u and all node v.

- Compute priority for each node defined within quorum set.  The terms within
hasNode() to create the random value include current proposed ledger hash,
previous closed ledger hash, round number, nodeid, and a priority boolean value.
*/
void
NominationProtocol::updateRoundLeaders()
{
    ZoneScoped;
    SCPQuorumSet myQSet = mSlot.getLocalNode()->getQuorumSet();

    auto localID = mSlot.getLocalNode()->getNodeID();
    if (myQSet.innerSets.size() > 0)
    {
        CLOG_DEBUG(SCP,
                   "** This node defines validators in different quality "
                   "groups.  High quality groups have a nested lower quality "
                   "group to be used as a backup for the higher quality group");
        for (xdr::xvector<stellar::PublicKey>::iterator it =
                 myQSet.validators.begin();
             it != myQSet.validators.end(); ++it)
        {
            auto value = *it;
            CLOG_DEBUG(SCP, "** Validator within innerset: {}",
                       mSlot.getSCPDriver().toShortString(value));
        }
    }
    else
    {
        CLOG_DEBUG(
            SCP,
            "** This node qset innerset is empty which means we don't define "
            "different quality (high, medium, low) validator groups.");
    }
    for (xdr::xvector<stellar::PublicKey>::iterator it =
             myQSet.validators.begin();
         it != myQSet.validators.end(); ++it)
    {
        auto value = *it;
        CLOG_DEBUG(SCP, "** Validator within this node qset: {}",
                   mSlot.getSCPDriver().toShortString(value));
    }

    normalizeQSet(myQSet, &localID); // excludes node u
    CLOG_DEBUG(SCP, "** START - After nomralizing my qset, here are now the "
                    "nodes within my qset: ");
    for (xdr::xvector<PublicKey>::iterator it = myQSet.validators.begin();
         it != myQSet.validators.end(); ++it)
    {
        auto value = *it;
        CLOG_DEBUG(SCP, "node v: {}",
                   mSlot.getSCPDriver().toShortString(value));
    }
    CLOG_DEBUG(SCP, "** END - After nomralizing my qset, here are now the "
                    "nodes within my qset: ");

    size_t maxLeaderCount = 1; // includes self (node v)
    // note that node IDs here are unique ("sane"), so we can count by
    // enumeration
    LocalNode::forAllNodes(myQSet, [&](NodeID const& cur) {
        ++maxLeaderCount;
        return true;
    });

    while (mRoundLeaders.size() < maxLeaderCount)
    {
        std::set<NodeID> newRoundLeaders;

        newRoundLeaders.insert(
            localID); // proposed leader is initialized with self (node u)
        uint64 topPriority = getNodePriorityWithoutWeight(
            localID, myQSet); // compute priority for node u
        CLOG_DEBUG(SCP,
                   "updateRoundLeaders: initializing node u {} to the new "
                   "leaders set.  Its priority is {}.  it may get kicked out "
                   "if peers have computed  higher priority.",
                   mSlot.getSCPDriver().toShortString(localID), topPriority);

        CLOG_DEBUG(SCP, "** updateRoundLeaders: start - processing "
                        "sequentially node Vs within qset: deriving "
                        "neighbors(u) and compute priority for each node.");
        LocalNode::forAllNodes(
            myQSet,
            [&](NodeID const& cur) { // for each node v, compute its priority.
                                     // Add top priority node v to leaders list
                uint64 w = getNodePriorityWithoutWeight(cur, myQSet);
                if (w > topPriority)
                {
                    CLOG_DEBUG(SCP,
                               "updateRoundLeaders: node v {} now is the most "
                               "important with priority value of {}.",
                               mSlot.getSCPDriver().toShortString(cur), w);
                    topPriority = w;
                    newRoundLeaders.clear();
                }
                if (w == topPriority && w > 0)
                {
                    CLOG_DEBUG(SCP,
                               "updateRoundLeaders:  adding top priority node "
                               "{} to new leaders set.",
                               mSlot.getSCPDriver().toShortString(cur));
                    newRoundLeaders.insert(cur);
                }
                return true;
            });
        CLOG_DEBUG(SCP, "** updateRoundLeaders: end - processing sequentially "
                        "nodes within qset.");

        // expand mRoundLeaders with the newly computed leaders
        auto oldSize = mRoundLeaders.size();
        mRoundLeaders.insert(
            newRoundLeaders.begin(),
            newRoundLeaders
                .end()); // expand mRoundLeaders with the newly computed leaders

        // Following logic is just to print out debug messages
        if (oldSize != mRoundLeaders.size())
        {
            if (Logging::logDebug("SCP"))
            {
                CLOG_DEBUG(SCP, "*** updateRoundLeaders: for this round, here "
                                "are who I think are the leader(s) (node v ) "
                                "in the list of MY neighbors (neighbors(u)).");
                for (auto const& rl : mRoundLeaders)
                {
                    CLOG_DEBUG(SCP, "    selected leader {}",
                               mSlot.getSCPDriver().toShortString(rl));
                }
            }
            return;
        }
        else
        {
            mRoundNumber++;
            CLOG_DEBUG(SCP, "updateRoundLeaders: the size of potential leaders "
                            "have not change, fast timeout (would no op)");
        }
    }
    CLOG_DEBUG(SCP, "updateRoundLeaders: nothing to do");
}

/*
Used by the nomination protocol to randomize the order of messages between
nodes, i.e. it tries to select a different node among the qset.

This function is the implementation of H0(V) or H1(v) mentioned in whitepaper
https://www.scs.stanford.edu/~dm/blog/simplified-scp.html.

H0(v) is a hash value used to determine if peer can be a "neighbor" of node u
(locla node). H1(v) is a hash value representing the priority for peer (node v).

The terms to create the random value include proposed ledger, previous closed
ledger, round number, nodeid, and a priority boolean value.

@param if true, H1(v) is computed; otherwise, H0(v).
@param node id of peer to compute hash value.
*/
uint64
NominationProtocol::hashNode(bool isPriority, NodeID const& nodeID)
{
    ZoneScoped;
    dbgAssert(!mPreviousValue.empty());
    stellar::uint64 result = mSlot.getSCPDriver().computeHashNode(
        mSlot.getSlotIndex(), mPreviousValue, isPriority, mRoundNumber, nodeID);
    if (isPriority)
    {
        CLOG_DEBUG(SCP,
                   "hasNode: computed priority H1(v) for node ({}) is ({})",
                   mSlot.getSCPDriver().toShortString(nodeID), result);
    }
    else
    {
        CLOG_DEBUG(SCP,
                   "hasNode: computed H0(V) for node ({}) is ({}).  Hash value "
                   "is part of deriving neighbors of node u.",
                   mSlot.getSCPDriver().toShortString(nodeID), result);
    }

    return result;
}

uint64
NominationProtocol::hashValue(Value const& value)
{
    ZoneScoped;
    dbgAssert(!mPreviousValue.empty());
    return mSlot.getSCPDriver().computeValueHash(
        mSlot.getSlotIndex(), mPreviousValue, mRoundNumber, value);
}

/*
Used by the nomination protocol to randomize the order of messages between
nodes, i.e. it tries to select a different node among the qset for each round of
nomination. The terms within hasNode() to create the random value include
current ledger index, previous close index, round number, nodeid, and a priority
boolean value.

NOTE: a peer's health (it's up or down, its load, etc) is  NOT consider in
computing a node's weight or priority. Thus you can elect a potential leader
that is currently down.  When this happens, the timeout logic kicks in.

@param nodeID node to copute priority
@param node u peers we trust
*/
uint64
NominationProtocol::getNodePriorityWithoutWeight(NodeID const& nodeID,
                                                 SCPQuorumSet const& qset)
{

    /* * * * * *
    This method staysclose to original logic within getNodePrirority() to allow
    for easier porting to future stellar protocol versions.
    * * * * * * */

    ZoneScoped;
    uint64 res;
    uint64 w;

    /**
    if (nodeID == mSlot.getLocalNode()->getNodeID()) //node v is the local node
    {
        // local node is in all quorum sets.
        // Remember, one rule in dynamic quorum slices is local node is always
    in all dynamically created slices. w = UINT64_MAX;
    }
    else
    {
        //node v is not local node so will compute its weight.
        w = LocalNode::getNodeWeight(nodeID, qset);
    }
    **/

    w = UINT64_MAX; // Take away advantage of local node to be elected leader.
    CLOG_DEBUG(SCP, "getNodePriorityWithoutWeight:  NOTE: all nodes set to max "
                    "value for its weight.");

    /*
    if w > 0; w is inclusive here as 0 <= hashNode <= UINT64_MAX.  Cmmputed hash
    value has to be less than computed weight. In our case, this condition is
    always true since we elminated any advantage of a local node.

    hasNode() is Used by the nomination protocol to randomize the order of
    messages between nodes, i.e. it tries to select a different node among the
    qset for each round.

    * hasNode() does not use node weight in its calculation.  Node weight comes
    into play as a boundary in the generated random number.  Specifically, the
    randomization number has to be > 0 and <= node weight, if not, randomization
    value is 0 which eliminates it from being a leader candidate.
    */
    if (w > 0 && hashNode(false, nodeID) <=
                     w) // hashNode(false, nodeID) is H0(v) in whitepaper.
    {
        res = hashNode(
            true,
            nodeID); // hasNode(true, nodeID) is H1(v).  priority(v) = H1(v)
    }
    else
    {
        // we should never be here

        res = 0;
        CLOG_ERROR(SCP,
                   "Unexpected confition, computed priority value ({}) is > "
                   "weight ({}) for node v ({})",
                   res, w, mSlot.getSCPDriver().toShortString(nodeID));
    }
    return res; // return computed priority H1(v) for peer
}

uint64
NominationProtocol::getNodePriority(NodeID const& nodeID,
                                    SCPQuorumSet const& qset)
{
    ZoneScoped;
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

    applyAll(nom, [&](Value const& value) {
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
    });
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

/*
attempts to nominate a value for consensus
To accomodate for leader failure, leader selection proceeds through rounds.

@value - current ledger to close.  Includes signature by self, hash of proposed
ledger, commit time.
@previousValue - previous ledger.  Includes signature by peer, hash of closed
ledger, commit time.
*/
bool
NominationProtocol::nominate(ValueWrapperPtr value, Value const& previousValue,
                             bool timedout)
{
    ZoneScoped;
    CLOG_DEBUG(SCP,
               "NominationProtocol::nominate - round: ({}), nomination value:  "
               "{}, \npreviousValue ({}",
               mRoundNumber, mSlot.getSCP().getValueString(value->getValue()),
               mSlot.getSCP().getValueString(previousValue));

    bool updated = false;

    if (timedout && !mNominationStarted)
    {
        CLOG_DEBUG(SCP, "NominationProtocol::nominate (TIMED OUT)");
        return false;
    }

    mNominationStarted = true;

    mPreviousValue = previousValue;

    mRoundNumber++;
    updateRoundLeaders(); // updates the set of nodes that have priority over
                          // the others.  Result is the leader(s) to nominate
                          // for this round.

    CLOG_DEBUG(
        SCP,
        "** START: NominationProtocol::nominate - after normalization, "
        "deriving \"neighbors\", and selecting neighbors with highest "
        "priority, here are the leader(s) I will nominate for this round.");
    for (auto roundLeader : mRoundLeaders)
    {
        CLOG_DEBUG(SCP, "Node ({})",
                   mSlot.getSCPDriver().toShortString(roundLeader));
    }
    CLOG_DEBUG(SCP, "** END: NominationProtocol::nominate - end.",
               mRoundNumber);

    /*
    If the selected leader appear not be fulfilling their responsibilies, then
    after a certain time out period nodes proceed to the next round to expand
    the set of leaders they follow.
    */
    std::chrono::milliseconds timeout = computeNominationTimeout(mRoundNumber);

    // if we're leader, add our value
    if (mRoundLeaders.find(mSlot.getLocalNode()->getNodeID()) !=
        mRoundLeaders.end())
    {
        auto ins = mVotes.insert(value);
        if (ins.second)
        {
            CLOG_DEBUG(SCP, "** NominationProtocol::nominate - found us in the "
                            "list of potential leaders, nominating self. ");
            updated = true;
            mSlot.getSCPDriver().nominatingValue(mSlot.getSlotIndex(),
                                                 value->getValue());
        }
    }
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
                CLOG_DEBUG(SCP,
                           "NominationProtocol::nominate - also nominating "
                           "node (from our potential leaders set)  {}",
                           mSlot.getSCPDriver().toShortString(leader));
                mVotes.insert(lnmV);
                updated = true;
                mSlot.getSCPDriver().nominatingValue(mSlot.getSlotIndex(),
                                                     lnmV->getValue());
            }
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
        CLOG_DEBUG(SCP, "NominationProtocol::nominate nominating value already "
                        "emitted so no need to emit again (SKIPPED)");
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

static const std::chrono::milliseconds NOMINATION_MAX_TIMEOUT_MS =
    std::chrono::milliseconds(1800000); // same value as MAX_TIMEOUT_SECONDS
                                        // defined within SCPDriver.cpp
std::chrono::milliseconds
NominationProtocol::computeNominationTimeout(uint32 roundNumber)
{
    if (leaderNominationTimeoutInMilisec >= NOMINATION_MAX_TIMEOUT_MS)
    {
        leaderNominationTimeoutInMilisec = NOMINATION_MAX_TIMEOUT_MS;
    }
    else
    {
        leaderNominationTimeoutInMilisec.operator+=(std::chrono::milliseconds(
            BASE_LEADER_NOMINATION_TIMEOUT_MS)); // increment by 250 ms for each
                                                 // round
    }
    return leaderNominationTimeoutInMilisec;
}
}
