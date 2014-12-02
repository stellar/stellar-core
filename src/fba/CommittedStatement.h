#ifndef __COMMITTEDSTATEMENT__
#define __COMMITTEDSTATEMENT__

#include "generated/stellar.hh"
#include "fba/Statement.h"
#include "fba/Ballot.h"

namespace stellar
{
    class CommittedStatement : public Statement
    {
    public:
        typedef std::shared_ptr<CommittedStatement> pointer;

        StatementType getType() { return(Statement::COMMITTED_TYPE); }

        CommittedStatement(stellarxdr::FBAEnvelope const& envelope);
        CommittedStatement(stellarxdr::uint256 const& nodeID, stellarxdr::uint256 const& qSetHash, Ballot::pointer ballot);
    };
}
#endif
