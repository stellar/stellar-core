#include <memory>
#include "generated/StellarXDR.h"

/*
	What FBA uses to talk to the rest of the world


	LATER: make a cleaner interface 

*/
namespace stellar
{
	class BallotValue
	{
	public:
		typedef std::shared_ptr<BallotValue> pointer;
		
		virtual stellarxdr::uint256 getContentsHash()=0;
		virtual bool operator > (const BallotValue& other)=0;
	};

	class FBAWorldInterface
	{

	public:

		// Ask the world if this is a legit value for a ballot
		virtual bool isValidX(BallotValue::pointer x)=0;

		// FBA has reached consensus on the value for this round. It is now safe to tell the world
		virtual void externalizeValue(BallotValue::pointer x) = 0;


	};

	

}