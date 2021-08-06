#include "TxSetCommutativityRequirements.h"

#include "ledger/LedgerTxnHeader.h"
#include "ledger/LedgerTxn.h"
#include "util/XDROperators.h"

namespace stellar {


AccountCommutativityRequirements& 
TxSetCommutativityRequirements::getRequirements(AccountID account)
{
	auto [iter, _] = mAccountRequirements.emplace(std::make_pair(account, account));
	return iter -> second;
}

bool 
TxSetCommutativityRequirements::canAddFee(LedgerTxnHeader& header, AbstractLedgerTxn& ltx, AccountID feeAccount, int64_t fee) 
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
TxSetCommutativityRequirements::tryAddTransaction(TransactionFrameBasePtr tx, AbstractLedgerTxn& ltx)
{

	LedgerTxnHeader header = ltx.loadHeader();

	if (!tx->isCommutativeTransaction()) {
		if (!canAddFee(header, ltx, tx->getFeeSourceID(), tx->getFeeBid())) {
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

	auto& prevReqs = getRequirements(sourceAccount);


	for (auto& req : reqs->getRequiredAssets()) {
		if (!prevReqs.checkAvailableBalanceSufficesForNewRequirement(header, ltx, req.first, req.second))
		{
			return false;
		}
	}

	if (!canAddFee(header, ltx, tx->getFeeSourceID(), tx->getFeeBid())) {
		return false;
	}

	for (auto& req : reqs->getRequiredAssets())
	{
		prevReqs.addAssetRequirement(req.first, req.second);
	}

	addFee(tx->getFeeSourceID(), tx->getFeeBid());

	return true;
}

bool 
TxSetCommutativityRequirements::tryReplaceTransaction(TransactionFrameBasePtr newTx, 
													  TransactionFrameBasePtr oldTx, 
													  AbstractLedgerTxn& ltx)
{

	AccountID sourceAccount = newTx->getSourceID();
	if (!(oldTx->getSourceID() == sourceAccount)) {
		throw std::logic_error("can't replace from different account");
	}

	auto newReqs = newTx -> getCommutativityRequirements(ltx);
	auto oldReqs = oldTx -> getCommutativityRequirements(ltx);

	if (newTx -> isCommutativeTransaction() && (!newReqs)) {
		return false;
	}

	auto newFeeBid = newTx -> getFeeBid();

	if (newTx -> getFeeSourceID() == oldTx -> getFeeSourceID())
	{
		newFeeBid -= oldTx -> getFeeBid();
	}
	if (newFeeBid < 0) {
		throw std::logic_error("replacement by fee should require an increase");
	}

	auto prevReqs = getRequirements(sourceAccount);

	LedgerTxnHeader header = ltx.loadHeader();

	if (newReqs)
	{
		if (oldReqs)
		{
			for (auto& oldReq : oldReqs -> getRequiredAssets())
			{
				newReqs->addAssetRequirement(oldReq.first, -oldReq.second);
			}
		}
		for (auto& newReq : newReqs -> getRequiredAssets())
		{
			if (!prevReqs.checkAvailableBalanceSufficesForNewRequirement(header, ltx, newReq.first, newReq.second)) {
				return false;
			}
		}
	}

	if (!canAddFee(header, ltx, newTx -> getFeeSourceID(), newFeeBid))
	{
		return false;
	}

	if (newReqs)
	{
		for (auto& req : newReqs -> getRequiredAssets())
		{
			prevReqs.addAssetRequirement(req.first, req.second);
		}
	}
	addFee(newTx->getFeeSourceID(), newFeeBid);

	if (!(newTx -> getFeeSourceID() == oldTx -> getFeeSourceID()))
	{
		addFee(oldTx -> getFeeSourceID(), -oldTx -> getFeeBid());
	}
	return true;
}

bool 
TxSetCommutativityRequirements::tryCleanAccountEntry(AccountID account)
{
	auto iter = mAccountRequirements.find(account);
	if (iter == mAccountRequirements.end())
	{
		return false;
	}
	iter -> second.cleanZeroedEntries();

	if (iter -> second.isEmpty())
	{
		mAccountRequirements.erase(iter);
		return true;
	}
	return false;

}



} /* ns stellar */
