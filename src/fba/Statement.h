#ifndef __FBAMESSAGE__
#define __FBAMESSAGE__

#include "txherder/TxHerderGateway.h"
#include "fba/FBA.h"

namespace stellar
{
    class Application;

	class Statement
	{
	protected:
		TxHerderGateway::BallotValidType mValidity;

        virtual void fillXDRBody(stellarxdr::FBAContents& body) { }
	public:
		typedef std::shared_ptr<Statement> pointer;

        stellarxdr::FBAEnvelope mEnvelope;
		stellarxdr::uint256 mContentsHash;  // TODO.1 should calculate this somewhere

       
		Statement();
        // creates a Statement from the wire
        Statement(stellarxdr::FBAEnvelope const& envelope);
		Statement(stellarxdr::FBAStatementType type, 
            stellarxdr::uint256 const& nodeID, 
            stellarxdr::uint256 const& qSetHash, 
            const stellarxdr::SlotBallot& ballot);

		void sign();

        stellarxdr::FBAStatementType getType() { return mEnvelope.contents.body.type(); }

        stellarxdr::SlotBallot& getSlotBallot() { return mEnvelope.contents.slotBallot; }
        stellarxdr::Ballot& getBallot() { return mEnvelope.contents.slotBallot.ballot; }



		bool operator == (Statement const& other)
		{
			return mContentsHash == other.mContentsHash;
		}

        bool isCompatible(BallotPtr ballot);

		
		TxHerderGateway::BallotValidType checkValidity(Application &app);

		bool compare(Statement::pointer other);

		// checks signature
		bool isSigValid();

		// check that it includes any tx you think are mandatory
		bool isTxSetValid();

		uint32_t getLedgerIndex();

		TransactionSetPtr fetchTxSet(Application &app);
	};

}

#endif
