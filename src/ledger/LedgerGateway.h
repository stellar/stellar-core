#ifndef __LEDGERGATEWAY__
#define __LEDGERGATEWAY__
#include "fba/FBA.h"

/*
Public Interface to the Ledger Module

*/

namespace stellar
{

	class LedgerGateway
	{
	public:
		// called by txherder
		virtual void externalizeValue(BallotPtr balllot, TransactionSet::pointer txSet)=0;

		// called by CLF
		virtual void ledgerHashComputed(stellarxdr::uint256& hash)=0;
	};
}

#endif