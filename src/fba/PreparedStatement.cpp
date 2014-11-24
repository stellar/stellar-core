#include "fba/PreparedStatement.h"

namespace stellar
{
    PreparedStatement::PreparedStatement(stellarxdr::FBAEnvelope& envelope) : Statement(envelope)
    {

    }

    PreparedStatement::PreparedStatement(stellarxdr::uint256& nodeID, stellarxdr::uint256& qSetHash, Ballot::pointer ballot) :
        Statement(nodeID, qSetHash, ballot)
    {

    }
}