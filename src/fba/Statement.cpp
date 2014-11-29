#include "Statement.h"
#include "main/Application.h"
#include "fba/Ballot.h"
#include "fba/CommitStatement.h"
#include "fba/CommittedStatement.h"
#include "fba/PreparedStatement.h"
#include "fba/PrepareStatement.h"

namespace stellar
{
    Statement::pointer Statement::makeStatement(stellarxdr::FBAEnvelope& envelope)
    {
        switch(envelope.contents.body.type())
        {
        case stellarxdr::PREPARE:
            return(Statement::pointer(new PrepareStatement(envelope)));
        case stellarxdr::PREPARED:
            return(Statement::pointer(new PreparedStatement(envelope)));
        case stellarxdr::COMMIT:
            return(Statement::pointer(new CommitStatement(envelope)));
        case stellarxdr::COMMITTED:
            return(Statement::pointer(new CommittedStatement(envelope)));
        }
        return(Statement::pointer());
    }

	Statement::Statement()
	{
		mValidity = TxHerderGateway::UKNOWN_VALIDITY;
	}

    Statement::Statement(stellarxdr::FBAEnvelope& envelope)
    {
        mValidity = TxHerderGateway::UKNOWN_VALIDITY;
        mBallot = Ballot::pointer(new Ballot(envelope.contents.ballot));
        mQuorumSetHash = envelope.contents.quorumSetHash;
        mNodeID = envelope.nodeID;
        mSignature = envelope.signature;
    }

	Statement::Statement(stellarxdr::uint256& nodeID, stellarxdr::uint256& qSetHash, Ballot::pointer ballot) : mNodeID(nodeID), mQuorumSetHash(qSetHash)
	{
		mBallot = ballot;
		mValidity = TxHerderGateway::UKNOWN_VALIDITY;
	}

    bool Statement::isCompatible(BallotPtr ballot)
    {
        return(mBallot->isCompatible(ballot));
    }
	void Statement::sign()
	{
		// SANITY
	}

	// checks signature
	bool Statement::isSigValid()
	{
		// SANITY
		return(true);
	}

	uint32_t Statement::getLedgerIndex(){ return(mBallot->mLederIndex); }


	TxHerderGateway::BallotValidType Statement::checkValidity(Application::pointer app)
	{
		if( mValidity == TxHerderGateway::UKNOWN_VALIDITY ||
			mValidity == TxHerderGateway::FUTURE_BALLOT)
		{
			mValidity=app->getTxHerderGateway().isValidBallotValue(mBallot);
		}
		return mValidity;
	}

	bool Statement::compare(Statement::pointer other)
	{
		return mBallot->compare(other->mBallot);
	}


	TransactionSet::pointer Statement::fetchTxSet(Application::pointer app)
	{
		return app->getTxHerderGateway().fetchTxSet(mBallot->mTxSetHash);
	}

    void Statement::toXDR(stellarxdr::FBAEnvelope& envelope)
    {
        envelope.nodeID = mNodeID;
        envelope.signature = mSignature;
        envelope.contents.quorumSetHash = mQuorumSetHash;
        mBallot->toXDR(envelope.contents.ballot);
        envelope.contents.body.type((stellarxdr::FBAStatementType)getType());
        fillXDRBody(envelope.contents);
    }

}