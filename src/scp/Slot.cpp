// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "Slot.h"

#include <cassert>
#include <functional>
#include "util/types.h"
#include "xdrpp/marshal.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "util/Logging.h"
#include "scp/Node.h"
#include "scp/LocalNode.h"
#include "lib/json/json.h"
#include "util/make_unique.h"

namespace stellar
{
using xdr::operator==;
using xdr::operator<;
using namespace std::placeholders;

Slot::Slot(uint64 slotIndex, SCP& scp)
    : mSlotIndex(slotIndex), mSCP(scp), mBallotProtocol(*this), mNominationProtocol(*this)
{
}

void
Slot::recordStatement(SCPStatement const& st)
{
    mStatementsHistory.emplace_back(st);
}

SCP::EnvelopeState
Slot::processEnvelope(SCPEnvelope const& envelope)
{
    assert(envelope.statement.slotIndex == mSlotIndex);

    CLOG(DEBUG, "SCP") << "Slot::processEnvelope"
                       << " i: " << getSlotIndex() << " " << envToStr(envelope);

    SCP::EnvelopeState res;

    if (envelope.statement.pledges.type() == SCPStatementType::SCP_ST_NOMINATE)
    {
        res = mNominationProtocol.processEnvelope(envelope);
    }
    else
    {
        res = mBallotProtocol.processEnvelope(envelope);
    }
    return res;
}

bool
Slot::abandonBallot()
{
    return mBallotProtocol.abandonBallot();
}

bool
Slot::bumpState(Value const& value)
{
    return mBallotProtocol.bumpState(value);
}

SCPEnvelope
Slot::createEnvelope(SCPStatement const& statement)
{
    SCPEnvelope envelope;

    envelope.statement = statement;
    auto& mySt = envelope.statement;
    mySt.nodeID = getSCP().getLocalNodeID();
    mySt.slotIndex = getSlotIndex();

    mSCP.signEnvelope(envelope);

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
        abort();
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
Slot::getQuorumSetFromStatement(SCPStatement const& st) const
{
    SCPQuorumSetPtr res;
    SCPStatementType t = st.pledges.type();

    if (t == SCP_ST_EXTERNALIZE)
    {
        res = Node::getSingletonQSet(st.nodeID);
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
            abort();
        }
        res = mSCP.getQSet(h);
    }
    return res;
}

void
Slot::dumpInfo(Json::Value& ret)
{
    Json::Value slotValue;
    slotValue["index"] = static_cast<int>(mSlotIndex);

    int count = 0;
    for (auto& item : mStatementsHistory)
    {
        slotValue["statements"][count++] = envToStr(item);
    }

    mBallotProtocol.dumpInfo(ret);

    ret["slot"].append(slotValue);
}

std::string
Slot::ballotToStr(SCPBallot const& ballot) const
{
    std::ostringstream oss;

    oss << "(" << ballot.counter << "," << mSCP.getValueString(ballot.value)
        << ")";
    return oss.str();
}

std::string
Slot::ballotToStr(std::unique_ptr<SCPBallot> const& ballot) const
{
    std::string res;
    if (ballot)
    {
        res = ballotToStr(*ballot);
    }
    else
    {
        res = "(<null_ballot>)";
    }
    return res;
}

std::string
Slot::envToStr(SCPEnvelope const& envelope) const
{
    std::ostringstream oss;
    return oss.str();
}

std::string
Slot::envToStr(SCPStatement const& st) const
{
    std::ostringstream oss;

    Hash qSetHash = getCompanionQuorumSetHashFromStatement(st);

    oss << "{ENV@" << hexAbbrev(st.nodeID) << " | "
        << " i: " << st.slotIndex;
    switch (st.pledges.type())
    {
    case SCPStatementType::SCP_ST_PREPARE:
    {
        auto const& p = st.pledges.prepare();
        oss << " | PREPARE"
            << " | D: " << hexAbbrev(qSetHash)
            << " | b: " << ballotToStr(p.ballot)
            << " | p: " << ballotToStr(p.prepared)
            << " | p': " << ballotToStr(p.preparedPrime) << " | nc: " << p.nC
            << " | nP: " << p.nP;
    }
    break;
    case SCPStatementType::SCP_ST_CONFIRM:
    {
        auto const& c = st.pledges.confirm();
        oss << " | COMMIT"
            << " | D: " << hexAbbrev(qSetHash) << " | np: " << c.nPrepared
            << " | c: " << ballotToStr(c.commit) << " | nP: " << c.nP;
    }
    break;
    case SCPStatementType::SCP_ST_EXTERNALIZE:
    {
        auto const& ex = st.pledges.externalize();
        oss << " | EXTERNALIZE"
            << " | c: " << ballotToStr(ex.commit) << " | nP: " << ex.nP
            << " | (lastD): " << hexAbbrev(qSetHash);
    }
    break;
    case SCPStatementType::SCP_ST_NOMINATE:
    {
        auto const& nom = st.pledges.nominate();
        oss << " | NOMINATE"
            << " | D: " << hexAbbrev(qSetHash) << " | X: {";
        for (auto const& v : nom.votes)
        {
            oss << " '" << mSCP.getValueString(v) << "',";
        }
        oss << "}"
            << " | Y: {";
        for (auto const& a : nom.accepted)
        {
            oss << " '" << mSCP.getValueString(a) << "',";
        }
        oss << "}";
    }
    break;
    }

    oss << " }";
    return oss.str();
}

bool
Slot::federatedAccept(StatementPredicate voted, StatementPredicate accepted,
                      std::map<uint256, SCPStatement> const& statements)
{
    // Checks if the nodes that claimed to accept the statement form a
    // v-blocking set
    if (getLocalNode()->isVBlocking<SCPStatement>(
            getLocalNode()->getQuorumSet(), statements, accepted))
    {
        return true;
    }

    // Checks if the set of nodes that accepted or voted for it form a quorum

    auto ratifyFilter =
        [this, &voted, &accepted](uint256 const& nodeID,
                                  SCPStatement const& st) -> bool
    {
        bool res;
        res = accepted(nodeID, st) || voted(nodeID, st);
        return res;
    };

    if (getLocalNode()->isQuorum<SCPStatement>(
            getLocalNode()->getQuorumSet(), statements,
            std::bind(&Slot::getQuorumSetFromStatement, this, _1),
            ratifyFilter))
    {
        return true;
    }

    return false;
}

bool
Slot::federatedRatify(StatementPredicate voted,
                      std::map<uint256, SCPStatement> const& statements)
{
    return getLocalNode()->isQuorum<SCPStatement>(
        getLocalNode()->getQuorumSet(), statements,
        std::bind(&Slot::getQuorumSetFromStatement, this, _1), voted);
}

std::shared_ptr<LocalNode>
Slot::getLocalNode()
{
    return mSCP.getLocalNode();
}
}
