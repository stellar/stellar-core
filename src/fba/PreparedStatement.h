#ifndef __PREPAREDSTATEMENT__
#define __PREPAREDSTATEMENT__

#include "generated/stellar.hh"
#include "fba/Statement.h"
#include "fba/Ballot.h"

namespace stellar
{


    class PreparedStatement : public Statement
    {
    public:
        typedef std::shared_ptr<PreparedStatement> pointer;

        StatementType getType() { return(PREPARED_TYPE); }

        PreparedStatement(stellarxdr::FBAEnvelope const& envelope);
        PreparedStatement(stellarxdr::uint256 const& nodeID, stellarxdr::uint256 const& qSetHash, Ballot::pointer ballot);


    };
}
#endif

