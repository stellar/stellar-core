#include "fba/PrepareStatement.h"

namespace stellar
{
    PrepareStatement::PrepareStatement(stellarxdr::uint256 const& nodeID, stellarxdr::uint256 const& qSetHash, Ballot::pointer ballot) : Statement(nodeID,qSetHash,ballot)
    {

    }

    // make from the Wire
    PrepareStatement::PrepareStatement(stellarxdr::FBAEnvelope const& msg) : Statement(msg)
    {
       
        for(auto excluded : msg.contents.body.excepted())
        {
            Ballot::pointer xBallot(new Ballot(excluded));
            mExcludedBallots.push_back(xBallot);
        }
    }

    void PrepareStatement::fillXDRBody(stellarxdr::FBAContents& contents)
    {
        contents.body.excepted().resize(mExcludedBallots.size());
        for(unsigned int n = 0; n < mExcludedBallots.size(); n++)
        {
            mExcludedBallots[n]->toXDR(contents.body.excepted()[n]);
        }
    }

}
