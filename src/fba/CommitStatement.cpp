#include "fba/CommitStatement.h"

namespace stellar
{
    CommitStatement::CommitStatement(stellarxdr::FBAEnvelope& envelope) : Statement(envelope)
    {
    }

    CommitStatement::CommitStatement(stellarxdr::uint256& nodeID, stellarxdr::uint256& qSetHash, Ballot::pointer ballot) :
        Statement(nodeID, qSetHash, ballot)
    {
    }
}
