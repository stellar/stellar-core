// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "Statement.h"
#include "main/Application.h"
#include "fba/Ballot.h"


namespace stellar
{
Statement::Statement() : mValidity(TxHerderGateway::UNKNOWN_VALIDITY)
{
}

// make from the wire
Statement::Statement(FBAEnvelope const& envelope)
    : mValidity(TxHerderGateway::UNKNOWN_VALIDITY), mEnvelope(envelope)
{
}

Statement::Statement(FBAStatementType type,
                     uint256 const& nodeID,
                     uint256 const& qSetHash,
                     const SlotBallot& ballot)
    : mValidity(TxHerderGateway::UNKNOWN_VALIDITY)
{
    mEnvelope.contents.body.type(type);
    mEnvelope.nodeID = nodeID;
    mEnvelope.contents.quorumSetHash = qSetHash;
    mEnvelope.contents.slotBallot = ballot;
}

bool
Statement::isCompatible(BallotPtr ballot)
{
    return (ballot::isCompatible(getBallot(), *ballot));
}
void
Statement::sign()
{
    // SANITY sign
}

// checks signature
bool
Statement::isSigValid()
{
    // SANITY check sig
    return (true);
}

uint32_t
Statement::getLedgerIndex()
{
    return (getSlotBallot().ledgerIndex);
}

TxHerderGateway::BallotValidType
Statement::checkValidity(Application& app)
{
    if (mValidity == TxHerderGateway::UNKNOWN_VALIDITY ||
        mValidity == TxHerderGateway::FUTURE_BALLOT)
    {
        mValidity = app.getTxHerderGateway().isValidBallotValue(getBallot());
    }
    return mValidity;
}

bool
Statement::compare(Statement::pointer other)
{
    return ballot::compare(getBallot(), other->getBallot());
}

TxSetFramePtr
Statement::fetchTxSet(Application& app)
{
    return app.getTxHerderGateway().fetchTxSet(getBallot().txSetHash, true);
}
}
