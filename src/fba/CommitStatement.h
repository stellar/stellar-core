#ifndef __COMMITSTATEMENT__
#define __COMMITSTATEMENT__

#include "generated/stellar.hh"
#include "fba/Statement.h"
#include "fba/Ballot.h"

namespace stellar
{
    class CommitStatement : public Statement
    {
    public:
        typedef std::shared_ptr<CommitStatement> pointer;

        StatementType getType() { return(COMMIT_TYPE); }

        CommitStatement(stellarxdr::FBAEnvelope const& envelope);
        CommitStatement(stellarxdr::uint256 const& nodeID, stellarxdr::uint256 const& qSetHash, Ballot::pointer ballot);
    };
}
#endif
