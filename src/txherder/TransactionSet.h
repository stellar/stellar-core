#ifndef __TRANSACTIONSET__
#define __TRANSACTIONSET__

#include "transactions/Transaction.h"
//#include "fba/FBAWorldInterface.h"
#include "generated/stellar.hh"

using namespace std;

namespace stellar
{
	class TransactionSet 
	{
		stellarxdr::uint256 mHash;
	public:
		typedef std::shared_ptr<TransactionSet> pointer;

		vector<Transaction::pointer> mTransactions;
		
        TransactionSet();

        // make it from the wire
        TransactionSet(stellarxdr::TransactionSet& xdrSet);

		Transaction::pointer getTransaction(stellarxdr::uint256& txHash);
		// returns the hash of this tx set
		stellarxdr::uint256 getContentsHash();

		void add(Transaction::pointer tx){ mTransactions.push_back(tx); }
		void serialize();

		void store();

		uint32_t size(){ return mTransactions.size(); }

        void toXDR(stellarxdr::TransactionSet& set);

	};


}

#endif // !__TRANSACTIONSET__