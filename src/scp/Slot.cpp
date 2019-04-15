// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "Slot.h"

#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "lib/json/json.h"
#include "main/ErrorMessages.h"
#include "scp/LocalNode.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/XDROperators.h"
#include "xdrpp/marshal.h"
#include <ctime>
#include <functional>

namespace stellar
{
using namespace std::placeholders;

Slot::Slot(uint64 slotIndex, SCP& scp)
    : mSlotIndex(slotIndex)
    , mSCP(scp)
    , mBallotProtocol(*this)
    , mNominationProtocol(*this)
    , mFullyValidated(scp.getLocalNode()->isValidator())
{
}

Value const&
Slot::getLatestCompositeCandidate()
{
    return mNominationProtocol.getLatestCompositeCandidate();
}

std::vector<SCPEnvelope>
Slot::getLatestMessagesSend() const
{
    std::vector<SCPEnvelope> res;
    if (mFullyValidated)
    {
        SCPEnvelope* e;
        e = mNominationProtocol.getLastMessageSend();
        if (e)
        {
            res.emplace_back(*e);
        }
        e = mBallotProtocol.getLastMessageSend();
        if (e)
        {
            res.emplace_back(*e);
        }
    }
    return res;
}

void
Slot::setStateFromEnvelope(SCPEnvelope const& e)
{
    if (e.statement.nodeID == getSCP().getLocalNodeID() &&
        e.statement.slotIndex == mSlotIndex)
    {
        if (e.statement.pledges.type() == SCPStatementType::SCP_ST_NOMINATE)
        {
            mNominationProtocol.setStateFromEnvelope(e);
        }
        else
        {
            mBallotProtocol.setStateFromEnvelope(e);
        }
    }
    else
    {
        if (Logging::logTrace("SCP"))
            CLOG(TRACE, "SCP")
                << "Slot::setStateFromEnvelope invalid envelope"
                << " i: " << getSlotIndex() << " " << mSCP.envToStr(e);
    }
}

std::vector<SCPEnvelope>
Slot::getCurrentState() const
{
    std::vector<SCPEnvelope> res;
    res = mNominationProtocol.getCurrentState();
    auto r2 = mBallotProtocol.getCurrentState();
    res.insert(res.end(), r2.begin(), r2.end());
    return res;
}

SCPEnvelope const*
Slot::getLatestMessage(NodeID const& id) const
{
    auto m = mBallotProtocol.getLatestMessage(id);
    if (m == nullptr)
    {
        m = mNominationProtocol.getLatestMessage(id);
    }
    return m;
}

std::vector<SCPEnvelope>
Slot::getExternalizingState() const
{
    return mBallotProtocol.getExternalizingState();
}

void
Slot::recordStatement(SCPStatement const& st)
{
    mStatementsHistory.emplace_back(
        HistoricalStatement{std::time(nullptr), st, mFullyValidated});
    CLOG(DEBUG, "SCP") << "new statement: "
                       << " i: " << getSlotIndex()
                       << " st: " << mSCP.envToStr(st, false) << " validated: "
                       << (mFullyValidated ? "true" : "false");
}

SCP::EnvelopeState
Slot::processEnvelope(SCPEnvelope const& envelope, bool self)
{
    dbgAssert(envelope.statement.slotIndex == mSlotIndex);

    if (Logging::logTrace("SCP"))
        CLOG(TRACE, "SCP") << "Slot::processEnvelope"
                           << " i: " << getSlotIndex() << " "
                           << mSCP.envToStr(envelope);

    SCP::EnvelopeState res;

    try
    {

        if (envelope.statement.pledges.type() ==
            SCPStatementType::SCP_ST_NOMINATE)
        {
            res = mNominationProtocol.processEnvelope(envelope);
        }
        else
        {
            res = mBallotProtocol.processEnvelope(envelope, self);
        }
    }
    catch (...)
    {
        CLOG(FATAL, "SCP") << "SCP context:";
        CLOG(FATAL, "SCP") << getJsonInfo().toStyledString();
        CLOG(FATAL, "SCP") << "Exception processing SCP messages at "
                           << mSlotIndex
                           << ", envelope: " << mSCP.envToStr(envelope);
        CLOG(FATAL, "SCP") << REPORT_INTERNAL_BUG;

        throw;
    }
    return res;
}

bool
Slot::abandonBallot()
{
    return mBallotProtocol.abandonBallot(0);
}

bool
Slot::bumpState(Value const& value, bool force)
{

    return mBallotProtocol.bumpState(value, force);
}

bool
Slot::nominate(Value const& value, Value const& previousValue, bool timedout)
{
    return mNominationProtocol.nominate(value, previousValue, timedout);
}

void
Slot::stopNomination()
{
    mNominationProtocol.stopNomination();
}

std::set<NodeID>
Slot::getNominationLeaders() const
{
    return mNominationProtocol.getLeaders();
}

bool
Slot::isFullyValidated() const
{
    return mFullyValidated;
}

void
Slot::setFullyValidated(bool fullyValidated)
{
    mFullyValidated = fullyValidated;
}

SCPEnvelope
Slot::createEnvelope(SCPStatement const& statement)
{
    SCPEnvelope envelope;

    envelope.statement = statement;
    auto& mySt = envelope.statement;
    mySt.nodeID = getSCP().getLocalNodeID();
    mySt.slotIndex = getSlotIndex();

    mSCP.getDriver().signEnvelope(envelope);

    return envelope;
}

Hash
Slot::getCompanionQuorumSetHashFromStatement(SCPStatement const& st)
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
    case SCP_ST_NOMINATE:
        h = st.pledges.nominate().quorumSetHash;
        break;
    default:
        dbgAbort();
    }
    return h;
}

std::vector<Value>
Slot::getStatementValues(SCPStatement const& st)
{
    std::vector<Value> res;
    if (st.pledges.type() == SCP_ST_NOMINATE)
    {
        res = NominationProtocol::getStatementValues(st);
    }
    else
    {
        res.emplace_back(BallotProtocol::getWorkingBallot(st).value);
    }
    return res;
}

SCPQuorumSetPtr
Slot::getQuorumSetFromStatement(SCPStatement const& st)
{
    SCPQuorumSetPtr res;
    SCPStatementType t = st.pledges.type();

    if (t == SCP_ST_EXTERNALIZE)
    {
        res = LocalNode::getSingletonQSet(st.nodeID);
    }
    else
    {
        Hash h;
        if (t == SCP_ST_PREPARE)
        {
            h = st.pledges.prepare().quorumSetHash;
        }
        else if (t == SCP_ST_CONFIRM)
        {
            h = st.pledges.confirm().quorumSetHash;
        }
        else if (t == SCP_ST_NOMINATE)
        {
            h = st.pledges.nominate().quorumSetHash;
        }
        else
        {
            dbgAbort();
        }
        res = getSCPDriver().getQSet(h);
    }
    return res;
}

Json::Value
Slot::getJsonInfo(bool fullKeys)
{
    Json::Value ret;
    std::map<Hash, SCPQuorumSetPtr> qSetsUsed;

    int count = 0;
    for (auto const& item : mStatementsHistory)
    {
        Json::Value& v = ret["statements"][count++];
        v.append((Json::UInt64)item.mWhen);
        v.append(mSCP.envToStr(item.mStatement, fullKeys));
        v.append(item.mValidated);

        Hash const& qSetHash =
            getCompanionQuorumSetHashFromStatement(item.mStatement);
        auto qSet = getSCPDriver().getQSet(qSetHash);
        if (qSet)
        {
            qSetsUsed.insert(std::make_pair(qSetHash, qSet));
        }
    }

    auto& qSets = ret["quorum_sets"];
    for (auto const& q : qSetsUsed)
    {
        qSets[hexAbbrev(q.first)] = getLocalNode()->toJson(*q.second, fullKeys);
    }

    ret["validated"] = mFullyValidated;
    ret["nomination"] = mNominationProtocol.getJsonInfo();
    ret["ballotProtocol"] = mBallotProtocol.getJsonInfo();

    return ret;
}

Json::Value
Slot::getJsonQuorumInfo(NodeID const& id, bool summary, bool fullKeys)
{
    Json::Value ret = mBallotProtocol.getJsonQuorumInfo(id, summary, fullKeys);
    if (getLocalNode()->isValidator())
    {
        ret["validated"] = isFullyValidated();
    }
    return ret;
}

bool
Slot::federatedAccept(StatementPredicate voted, StatementPredicate accepted,
                      std::map<NodeID, SCPEnvelope> const& envs)
{
    // Checks if the nodes that claimed to accept the statement form a
    // v-blocking set
    if (LocalNode::isVBlocking(getLocalNode()->getQuorumSet(), envs, accepted))
    {
        return true;
    }

    // Checks if the set of nodes that accepted or voted for it form a quorum

    auto ratifyFilter = [&](SCPStatement const& st) {
        bool res;
        res = accepted(st) || voted(st);
        return res;
    };

    if (LocalNode::isQuorum(
            getLocalNode()->getQuorumSet(), envs,
            std::bind(&Slot::getQuorumSetFromStatement, this, _1),
            ratifyFilter))
    {
        return true;
    }

    return false;
}

bool
Slot::federatedRatify(StatementPredicate voted,
                      std::map<NodeID, SCPEnvelope> const& envs)
{
    return LocalNode::isQuorum(
        getLocalNode()->getQuorumSet(), envs,
        std::bind(&Slot::getQuorumSetFromStatement, this, _1), voted);
}

std::shared_ptr<LocalNode>
Slot::getLocalNode()
{
    return mSCP.getLocalNode();
}

std::vector<SCPEnvelope>
Slot::getEntireCurrentState()
{
    bool old = mFullyValidated;
    // fake fully validated to force returning all envelopes
    mFullyValidated = true;
    auto r = getCurrentState();
    mFullyValidated = old;
    return r;
}
}
