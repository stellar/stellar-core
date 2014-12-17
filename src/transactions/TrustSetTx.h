#ifndef __TRUSTSETTX__
#define __TRUSTSETTX__

#include "Transaction.h"

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

namespace stellar
{
	class TrustSetTx : public Transaction
	{
		TxResultCode doApply();
	public:
		stellarxdr::uint160 mCurrency;
        stellarxdr::uint160 mOtherAccount;
		uint64_t mYourlimt;
		bool mAuthSet;
	};
}

#endif
