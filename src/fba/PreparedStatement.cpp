#include "fba/PreparedStatement.h"

namespace stellar
{
    PreparedStatement::PreparedStatement(stellarxdr::FBAEnvelope const& envelope) : Statement(envelope)
    {

    }

    PreparedStatement::PreparedStatement(stellarxdr::uint256 const& nodeID, stellarxdr::uint256 const& qSetHash, Ballot::pointer ballot) :
        Statement(nodeID, qSetHash, ballot)
    {

    }
}
