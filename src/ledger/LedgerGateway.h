#ifndef __LEDGERGATEWAY__
#define __LEDGERGATEWAY__
/*
Public Interface to the Ledger Module

*/

namespace stellar
{

	class LedgerGateway
	{
	public:
		// called by txherder
		virtual void externalizeValue(TransactionSet::pointer txSet,uint64_t closeTime)=0;

		// called by CLF
		virtual void ledgerHashComputed(stellarxdr::uint256& hash)=0;
	};
}

#endif