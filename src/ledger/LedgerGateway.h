#ifndef __LEDGERGATEWAY__
#define __LEDGERGATEWAY__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "fba/FBA.h"
#include "clf/CLF.h"
#include "txherder/TransactionSet.h"

/*
Public Interface to the Ledger Module

*/

namespace stellar
{
    class TransactionSet;
    typedef std::shared_ptr<TransactionSet> TransactionSetPtr;

	class LedgerGateway
	{
	public:
		// called by txherder
		virtual void externalizeValue(const stellarxdr::SlotBallot& slotBallot, TransactionSetPtr txSet)=0;

		// called by CLF
        virtual void recvDelta(CLFDeltaPtr delta, LedgerHeaderPtr header) = 0;
	};
}

#endif
