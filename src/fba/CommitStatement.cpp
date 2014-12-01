#include "fba/CommitStatement.h"

namespace stellar
{
    CommitStatement::CommitStatement(stellarxdr::FBAEnvelope const& envelope) : Statement(envelope)
    {
    }

    CommitStatement::CommitStatement(stellarxdr::uint256 const& nodeID, stellarxdr::uint256 const& qSetHash, Ballot::pointer ballot) :
        Statement(nodeID, qSetHash, ballot)
    {
    }
}
