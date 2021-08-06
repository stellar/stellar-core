#include "TxSetCommutativityRequirements.h"

namespace stellar {


AccountCommutativityRequirements& 
TxSetCommutativityRequirements::getRequirements(AccountID account)
{
	auto [iter, _] = mAccountRequirements.emplace(std::make_pair(account, account));
	return iter -> second;
}

bool 
TxSetCommutativityRequirements::tryAddFee(LedgerTxnHeader& header, AbstractLedgerTxn& ltx, AccountID feeAccount, int64_t fee) 
{

	//Possibly redundant check
	auto accountEntry = stellar::loadAccount(ltx, feeAccount);
	if (!accountEntry)
	{
		return false;
	}

	auto& reqs = getRequirements(feeAccount);
	if (!reqs.checkAvailableBalanceSufficesForNewRequirement(header, ltx, getNativeAsset(), fee))
	{
		return false;
	}
	return true;
}

void 
TxSetCommutativityRequirements::addFee(AccountID feeAccount, int64_t fee) 
{
	getRequirements(feeAccount).addAssetRequirement(getNativeAsset(), fee);
}


bool 
TxSetCommutativityRequirements::tryAddTransaction(TransactionFrameBasePtr tx, LedgerTxnHeader& header, AbstractLedgerTxn& ltx)
{
	if (!tx->isCommutativeTransaction()) {
		if (!tryAddFee(header, ltx, tx->getFeeSourceID(), tx->getFeeBid())) {
			return false;
		}
		addFee(tx->getFeeSourceID(), tx->getFeeBid());
		return true;
	}

	auto reqs = tx->getCommutativityRequirements(ltx);
	if (!reqs) {
		return false;
	}

	auto sourceAccount = tx->getSourceID();

	//auto [reqs_iter, _] = mAccountRequirements.emplace(std::make_pair(sourceAccount, sourceAccount));

	auto& prevReqs = getRequirements(sourceAccount);//reqs_iter -> second;//mAccountRequirements[sourceAccount];

	for (auto& req : reqs->getRequiredAssets()) {
		if (!prevReqs.checkAvailableBalanceSufficesForNewRequirement(header, ltx, req.first, req.second))
		{
			return false;
		}
	}

	if (!tryAddFee(header, ltx, tx->getFeeSourceID(), tx->getFeeBid())) {
		return false;
	}

	for (auto& req : reqs->getRequiredAssets())
	{
		prevReqs.addAssetRequirement(req.first, req.second);
	}

	addFee(tx->getFeeSourceID(), tx->getFeeBid());

	return true;
}



} /* ns stellar */
