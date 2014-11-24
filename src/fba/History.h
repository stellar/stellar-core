#include "ledger/Ledger.h"

/*
Goals:
- cleaner state for stellard
-When we start know where we are in the Consensus process
-Don't start validating if we aren't synced



Collect Txns into a TX set

If a Txn comes in from a peer we don't bother to check it
If a Txn comes in from a client we check it against the LCL
When Txns come in we check them against the LCL
Server config if we check Txns or not?


When we make a proposal it is LedgerHash + TransactionSet
When a ledger closes we write its TxSet to disk

*/

/*
There are several records:
- SQL that tracks the current ledger
- SQL that stores the current ledger index
- SQL that stores the ledger history
	- Ledger header
	- tx set hash
- SQL of cached data to handle user requests
	- SQL of past transactions
- CLF that has the current ledger in serialized form
- Nodestore of past tx sets

*/

namespace stellar
{
	/*
	This node's interface to the Ledger history.
	*/
	class History
	{
	public:
		void loadLastKnownLedger(Ledger::pointer& ledger);
		void saveLedger(Ledger::pointer& ledger);

		void fetchHistoryFromNetwork();

	};

};
