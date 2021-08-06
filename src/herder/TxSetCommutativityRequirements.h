#pragma once

#include "herder/AccountCommutativityRequirements.h"

#include "transactions/TransactionFrameBase.h"

#include "ledger/LedgerHashUtils.h"

#include <map>


namespace stellar {

class AbstractLedgerTxn;

class TxSetCommutativityRequirements {

	std::map<AccountID, AccountCommutativityRequirements> mAccountRequirements;

	bool
	canAddFee(LedgerTxnHeader& header, AbstractLedgerTxn& ltx, AccountID feeAccount, int64_t fee);

	void addFee(AccountID feeAccount, int64_t fee);

	AccountCommutativityRequirements& 
	getRequirements(AccountID account);


public:

	bool tryAddTransaction(TransactionFrameBasePtr tx, AbstractLedgerTxn& ltx);

	bool tryReplaceTransaction(TransactionFrameBasePtr newTx, TransactionFrameBasePtr oldTx, AbstractLedgerTxn& ltx);

	// returns true if account has been removed from the map
	bool tryCleanAccountEntry(AccountID account);


};

} /* namespace stellar */
