#pragma once

#include "herder/AccountCommutativityRequirements.h"

#include "transactions/TransactionFrameBase.h"

#include "ledger/LedgerHashUtils.h"

#include <map>


namespace stellar {

class TxSetCommutativityRequirements {

	std::map<AccountID, AccountCommutativityRequirements> mAccountRequirements;

	bool
	tryAddFee(LedgerTxnHeader& header, AbstractLedgerTxn& ltx, AccountID feeAccount, int64_t fee);

	void addFee(AccountID feeAccount, int64_t fee);

	AccountCommutativityRequirements& 
	getRequirements(AccountID account);


public:

	bool tryAddTransaction(TransactionFrameBasePtr tx, LedgerTxnHeader& header, AbstractLedgerTxn& ltx);


};

} /* namespace stellar */
