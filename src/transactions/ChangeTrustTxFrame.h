#ifndef __TRUSTSETTX__
#define __TRUSTSETTX__

#include "transactions/TransactionFrame.h"

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

namespace stellar
{
	class ChangeTrustTxFrame : public TransactionFrame
	{
        
	public:
        ChangeTrustTxFrame(const TransactionEnvelope& envelope);

        void doApply(TxDelta& delta, LedgerMaster& ledgerMaster);
        bool doCheckValid(Application& app);
	};
}

#endif
