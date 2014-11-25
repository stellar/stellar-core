#include "fba/CommittedStatement.h"

namespace stellar
{
    CommittedStatement::CommittedStatement(stellarxdr::FBAEnvelope& envelope) : Statement(envelope)
    {
    }

    CommittedStatement::CommittedStatement(stellarxdr::uint256& nodeID, stellarxdr::uint256& qSetHash, Ballot::pointer ballot) :
        Statement(nodeID, qSetHash, ballot)
    {

    }
}

