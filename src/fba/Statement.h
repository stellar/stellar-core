#ifndef __FBAMESSAGE__
#define __FBAMESSAGE__

#include "txherder/TxHerderGateway.h"

namespace stellar
{
	class Ballot;
	typedef std::shared_ptr<Ballot> BallotPtr;

	class Statement
	{
	protected:
		TxHerderGateway::BallotValidType mValidity;

        virtual void fillXDRBody(stellarxdr::FBAContents& body) { }
	public:
		typedef std::shared_ptr<Statement> pointer;
		
		stellarxdr::uint256 mNodeID;
		stellarxdr::uint256 mSignature;
		stellarxdr::uint256 mContentsHash;
		stellarxdr::uint256 mQuorumSetHash;

		
		BallotPtr mBallot;

		enum StatementType {
			PREPARE_TYPE,
			PREPARED_TYPE,
			COMMIT_TYPE,
			COMMITTED_TYPE,
			NUM_TYPES,
			EXTERNALIZED_TYPE,   // ok this is ugly. we only need these for the node mState
			UNKNOWN_TYPE
		};

        // creates a Statement from the wire
        static Statement::pointer makeStatement(stellarxdr::FBAEnvelope& envelope);
		Statement();
        Statement(stellarxdr::FBAEnvelope& envelope);
		Statement(stellarxdr::uint256& nodeID, stellarxdr::uint256& qSetHash, BallotPtr ballot);

		void sign();

		virtual StatementType getType() = 0;



		bool operator == (Statement const& other)
		{
			return mContentsHash == other.mContentsHash;
		}

        bool isCompatible(BallotPtr ballot);

		
		TxHerderGateway::BallotValidType checkValidity();
		bool compare(Statement::pointer other);

		// checks signature
		bool isSigValid();

		// check that it includes any tx you think are mandatory
		bool isTxSetValid();

		uint32_t getLedgerIndex();

		TransactionSetPtr fetchTxSet();

		virtual void toXDR(stellarxdr::FBAEnvelope& envelope);	
	};

}

#endif