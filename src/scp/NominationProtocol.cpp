// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "NominationProtocol.h"

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
#include "Slot.h"

namespace stellar
{
using xdr::operator==;
using xdr::operator<;
using namespace std::placeholders;

NominationProtocol::NominationProtocol(Slot& slot)
    : mSlot(slot), mRoundNumber(0), mNominationStarted(false)
{
    updateRoundLeaders();
}

bool
NominationProtocol::isNewerStatement(uint256 const& nodeID,
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
        res = isNewerStatement(oldp->second.pledges.nominate(), st);
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
NominationProtocol::isValid(SCPStatement const& st)
{
    auto const& nom = st.pledges.nominate();
    bool res = (nom.votes.size() + nom.accepted.size()) != 0;

    res = res && std::is_sorted(nom.votes.begin(), nom.votes.end());
    res = res && std::is_sorted(nom.accepted.begin(), nom.accepted.end());

    applyAll(nom, [&](Value const& val)
             {
                 res =
                     res &&
                     mSlot.getSCP().validateValue(st.slotIndex, st.nodeID, val);
             });

    return res;
}

void
NominationProtocol::recordStatement(SCPStatement const& st)
{
    auto oldp = mLatestNominations.find(st.nodeID);
    if (oldp == mLatestNominations.end())
    {
        mLatestNominations.insert(std::make_pair(st.nodeID, st));
    }
    else
    {
        oldp->second = st;
    }
    mSlot.recordStatement(st);
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

    if (processEnvelope(envelope) == SCP::EnvelopeState::VALID)
    {
        if (!mLastEnvelope ||
            isNewerStatement(mLastEnvelope->statement.pledges.nominate(),
                             st.pledges.nominate()))
        {
            mLastEnvelope = make_unique<SCPEnvelope>(envelope);
            mSlot.getSCP().emitEnvelope(envelope);
        }
    }
    else
    {
        // there is a bug in the application if it queued up
        // a statement for itself that it considers invalid
        throw std::runtime_error("moved to a bad state");
    }
}

bool
NominationProtocol::acceptPredicate(Value const& v, uint256 const&,
                                    SCPStatement const& st)
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
    mRoundLeaders.clear();
    uint64 topPriority = 0;
    SCPQuorumSet const& myQSet = mSlot.getLocalNode()->getQuorumSet();

    Node::forAllNodes(myQSet, [&](uint256 const& cur)
                      {
                          uint64 w = getNodePriority(cur, myQSet);
                          if (w > topPriority)
                          {
                              topPriority = w;
                              mRoundLeaders.clear();
                          }
                          if (w == topPriority)
                          {
                              mRoundLeaders.insert(cur);
                          }
                      });
}

uint64
NominationProtocol::hashValue(bool isPriority, uint256 const& nodeID)
{
    return mSlot.getSCP().computeHash(mSlot.getSlotIndex(), isPriority,
                                      mRoundNumber, nodeID);
}

uint64
NominationProtocol::getNodePriority(uint256 const& nodeID,
                                    SCPQuorumSet const& qset)
{
    uint64 res;
    uint64 w = Node::getNodeWeight(nodeID, qset);

    if (hashValue(false, nodeID) < w)
    {
        res = hashValue(true, nodeID);
    }
    else
    {
        res = 0;
    }
    return res;
}

SCP::EnvelopeState
NominationProtocol::processEnvelope(SCPEnvelope const& envelope)
{
    auto const& st = envelope.statement;
    auto const& nom = st.pledges.nominate();

    SCP::EnvelopeState res = SCP::EnvelopeState::INVALID;

    if (isNewerStatement(st.nodeID, nom))
    {
        if (isValid(st))
        {
            recordStatement(st);
            res = SCP::EnvelopeState::VALID;

            bool modified = false; // tracks if we should emit a new nomination message
            bool newCandidates = false;

            // attempts to promote some of the votes to accepted
            for (auto const& v : nom.votes)
            {
                if (mAccepted.find(v) != mAccepted.end())
                {
                    continue;
                }
                if (mSlot.federatedAccept(
                        [&v](uint256 const&, SCPStatement const& st) -> bool
                        {
                            auto const& nom = st.pledges.nominate();
                            bool res;
                            res = (std::find(nom.votes.begin(), nom.votes.end(),
                                             v) != nom.votes.end());
                            return res;
                        },
                        std::bind(&NominationProtocol::acceptPredicate, v, _1,
                                  _2),
                        mLatestNominations))
                {
                    mAccepted.insert(v);
                    modified = true;
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
                        std::bind(&NominationProtocol::acceptPredicate, a, _1,
                                  _2),
                        mLatestNominations))
                {
                    mCandidates.insert(a);
                    newCandidates = true;
                }
            }

            // only take round leader votes if we still looking for candidates
            if (mCandidates.empty() &&
                mRoundLeaders.find(st.nodeID) != mRoundLeaders.end())
            {
                // see if we should update our list of votes
                for (auto const& v : nom.votes)
                {
                    if (mVotes.find(v) == mVotes.end())
                    {
                        mVotes.emplace(v);
                        modified = true;
                    }
                }
            }

            // don't participate in nomination if we don't have a value
            if (mNominationStarted)
            {
                if (modified)
                {
                    emitNomination();
                }

                if (newCandidates)
                {
                    Value v = mSlot.getSCP().combineCandidates(
                        mSlot.getSlotIndex(), mCandidates);
                    mSlot.bumpState(v);
                }
            }
        }
    }
    return res;
}

std::vector<Value>
NominationProtocol::getStatementValues(SCPStatement const& st)
{
    std::vector<Value> res;
    applyAll(st.pledges.nominate(), [&](Value const& v)
             {
                 res.emplace_back(v);
             });
    return res;
}

// attempts to nominate a value for consensus
bool
NominationProtocol::nominate(Value const& value, bool timedout)
{
    bool updated = false;

    mNominationStarted = true;

    if (timedout)
    {
        mRoundNumber++;
        updateRoundLeaders();
    }

    if (mRoundLeaders.find(mSlot.getLocalNode()->getNodeID()) !=
        mRoundLeaders.end())
    {
        auto ins = mVotes.insert(value);
        if (ins.second)
        {
            updated = true;
        }
    }
    else
    {
        for (auto const& leader : mRoundLeaders)
        {
            auto it = mLatestNominations.find(leader);
            if (it != mLatestNominations.end())
            {
                applyAll(it->second.pledges.nominate(), [&](Value const& value)
                         {
                             auto ins = mVotes.insert(value);
                             if (ins.second)
                             {
                                 updated = true;
                             }
                         });
            }
        }
    }

    if (timedout)
    {
        std::chrono::milliseconds timeout =
            std::chrono::seconds(mRoundNumber * (mRoundNumber + 1) / 2);

        if (updated)
        {
            Value nominatingValue =
                mSlot.getSCP().combineCandidates(mSlot.getSlotIndex(), mVotes);
            mSlot.getSCP().nominatingValue(mSlot.getSlotIndex(),
                                           nominatingValue, timeout);
        }
    }

    if (updated)
    {
        emitNomination();
    }

    return updated;
}
}
