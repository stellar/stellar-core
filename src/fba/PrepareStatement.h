#ifndef __PREPARESTATEMENT__
#define __PREPARESTATEMENT__

#include "generated/stellar.hh"
#include "fba/Statement.h"
#include "fba/Ballot.h"
#include "generated/stellar.hh"

namespace stellar
{
    class PrepareStatement : public Statement
    {
        void fillXDRBody(stellarxdr::FBAContents& body);
    public:
        typedef std::shared_ptr<PrepareStatement> pointer;

        StatementType getType() { return(Statement::PREPARE_TYPE); }

        vector<Ballot::pointer> mExcludedBallots; // these are ones this node has already set a COMMIT for

        PrepareStatement(stellarxdr::uint256 const& nodeID, stellarxdr::uint256 const& qSetHash, Ballot::pointer ballot);
        PrepareStatement(stellarxdr::FBAEnvelope const& msg);

    };
}

#endif
