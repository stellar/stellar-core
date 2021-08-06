#include "herder/AccountCommutativityRequirements.h"

namespace stellar
{

// TODO cache balances and trustline checks?

bool
AccountCommutativityRequirements::checkTrustLine(
	AbstractLedgerTxn& ltx, AccountID account, Asset asset) const
{
	if (asset.type() == ASSET_TYPE_NATIVE)
	{
		return true;
	}

	auto tl = loadTrustLine(ltx, account, asset);

	if (!tl) {
		return false;
	}

	return tl.isCommutativeTxEnabledTrustLine();
}

bool 
AccountCommutativityRequirements::checkCanAddAssetRequirement(
	AbstractLedgerTxn& ltx, Asset asset, int64_t amount) 
{

	if (amount < 0) {
		return false;
	}

	if (!checkTrustLine(ltx, mSourceAccount, asset)) {
		return false;
	}

	auto const& currentRequirement = mRequiredAssets[asset];

	if (INT64_MAX - currentRequirement < amount) {
		return false;
	}
	return true;
}


bool
AccountCommutativityRequirements::tryAddAssetRequirement(
	AbstractLedgerTxn& ltx, Asset asset, int64_t amount)
{
	if (!checkCanAddAssetRequirement(ltx, asset, amount)) {
		return false;
	}

	auto& currentRequirement = mRequiredAssets[asset];

	currentRequirement += amount;
	return true;
}
void
AccountCommutativityRequirements::addAssetRequirement(
	Asset asset, int64_t amount)
{
	mRequiredAssets[asset] += amount;
}

bool
AccountCommutativityRequirements::checkAvailableBalanceSufficesForNewRequirement(
	LedgerTxnHeader& header, AbstractLedgerTxn& ltx, Asset asset, int64_t amount)
{
	if (!checkCanAddAssetRequirement(ltx, asset, amount)) {
		return false;
	}
	auto& currentRequirement = mRequiredAssets[asset];

	auto currentBalance = getAvailableBalance(header, ltx, mSourceAccount, asset);

	if (amount + currentRequirement <= currentBalance) {
		return true;
	}
	return false;
}

int64_t 
AccountCommutativityRequirements::getNativeAssetReqs() {
	return mRequiredAssets[getNativeAsset()];
}



} /* stellar */