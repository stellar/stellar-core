#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <memory>
#include <functional>
#include <string>
#include <set>
#include <utility>
#include "scp/SCP.h"
#include "lib/json/json-forwards.h"

namespace stellar
{
class NominationProtocol
{
    Slot& mSlot;

    std::set<Value> mVotes;                           // X
    std::set<Value> mAccepted;                        // Y
    std::set<Value> mCandidates;                      // Z
    std::map<uint256, SCPStatement> mLatestNominations; // N

    std::unique_ptr<SCPEnvelope>
        mLastEnvelope; // last envelope emitted by this node

    bool isNewerStatement(uint256 const& nodeID, SCPNomination const& st);
    bool isNewerStatement(SCPNomination const& oldst, SCPNomination const& st);

    // returns true if 'p' is a subset of 'v'
    // also sets 'notEqual' if p and v differ
    // note: p and v must be sorted
    bool isSubsetHelper(xdr::xvector<Value> const& p,
                        xdr::xvector<Value> const& v, bool notEqual);

    bool isValid(SCPStatement const& st);

    void recordStatement(SCPStatement const& st);

    void emitNomination();

    // returns true if the statement asserts that v is in the accepted list
    static bool acceptPredicate(Value const&v, uint256 const&, SCPStatement const& st);
  public:
    NominationProtocol(Slot& slot);

    SCP::EnvelopeState processEnvelope(SCPEnvelope const& envelope);

    static std::vector<Value> getStatementValues(SCPStatement const& st);
};
}
