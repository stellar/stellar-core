#pragma once

#include "transactions/TransactionFrame.h"

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

namespace stellar
{
	class ChangeTrustTxFrame : public TransactionFrame
	{
        ChangeTrust::ChangeTrustResult &innerResult() { return mResult.body.tr().changeTrustResult(); }
	public:
        ChangeTrustTxFrame(const TransactionEnvelope& envelope);

        bool doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster);
        bool doCheckValid(Application& app);
	};

    namespace ChangeTrust
    {
        inline ChangeTrust::ChangeTrustResultCode getInnerCode(TransactionResult const & res)
        {
            return res.body.tr().changeTrustResult().code();
        }
    }

}
