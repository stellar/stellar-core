#include "fba/CommittedStatement.h"

namespace stellar
{
    CommittedStatement::CommittedStatement(stellarxdr::FBAEnvelope const& envelope) : Statement(envelope)
    {
    }

    CommittedStatement::CommittedStatement(stellarxdr::uint256 const& nodeID, stellarxdr::uint256 const& qSetHash, Ballot::pointer ballot) :
        Statement(nodeID, qSetHash, ballot)
    {

    }
}

