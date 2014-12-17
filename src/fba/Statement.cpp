#include "Statement.h"
#include "main/Application.h"
#include "fba/Ballot.h"


namespace stellar
{
Statement::Statement() : mValidity(TxHerderGateway::UNKNOWN_VALIDITY)
{
}

// make from the wire
Statement::Statement(stellarxdr::FBAEnvelope const& envelope)
    : mValidity(TxHerderGateway::UNKNOWN_VALIDITY), mEnvelope(envelope)
{
}

Statement::Statement(stellarxdr::FBAStatementType type,
                     stellarxdr::uint256 const& nodeID,
                     stellarxdr::uint256 const& qSetHash,
                     const stellarxdr::SlotBallot& ballot)
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

TransactionSet::pointer
Statement::fetchTxSet(Application& app)
{
    return app.getTxHerderGateway().fetchTxSet(getBallot().txSetHash, true);
}
}
