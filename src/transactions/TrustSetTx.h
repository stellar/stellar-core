#include "Transaction.h"

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