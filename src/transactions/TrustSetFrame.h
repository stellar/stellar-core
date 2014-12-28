#ifndef __TRUSTSETTX__
#define __TRUSTSETTX__

#include "transactions/TransactionFrame.h"

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

namespace stellar
{
	class TrustSetFrame : public TransactionFrame
	{
        void fillTrustLine(TrustFrame& line);
	public:
        void doApply(TxDelta& delta, LedgerMaster& ledgerMaster);
        bool doCheckValid(LedgerMaster& ledgerMaster);
	};
}

#endif
