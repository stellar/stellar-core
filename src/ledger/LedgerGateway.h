#ifndef __LEDGERGATEWAY__
#define __LEDGERGATEWAY__
#include "fba/FBA.h"
#include "clf/CLF.h"

/*
Public Interface to the Ledger Module

*/

namespace stellar
{

	class LedgerGateway
	{
	public:
		// called by txherder
		virtual void externalizeValue(BallotPtr ballot, TransactionSet::pointer txSet)=0;

		// called by CLF
        virtual void recvDelta(CLFDeltaPtr delta, LedgerHeaderPtr header) = 0;
	};
}

#endif