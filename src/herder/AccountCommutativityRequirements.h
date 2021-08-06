#pragma once

#include "ledger/TrustLineWrapper.h"
#include "util/UnorderedMap.h"
#include "transactions/TransactionUtils.h"
#include "xdr/Stellar-types.h"
#include "xdr/Stellar-transaction.h"
#include "xdr/Stellar-ledger-entries.h"

#include "util/XDROperators.h"

#include "ledger/LedgerHashUtils.h"

namespace stellar
{

class AbstractLedgerTxn;
class LedgerTxnHeader;


class AccountCommutativityRequirements {

	AccountID mSourceAccount;

	using AssetMap = UnorderedMap<Asset, int64_t>;

	AssetMap mRequiredAssets;

	bool checkCanAddAssetRequirement(
		AbstractLedgerTxn& ltx, Asset asset, int64_t amount);


public:

	AccountCommutativityRequirements(AccountID source) : mSourceAccount(source) {}

	bool checkTrustLine(AbstractLedgerTxn& ltx, AccountID account, Asset asset) const;

	//implicitly checks trustline
	bool tryAddAssetRequirement(AbstractLedgerTxn& ltx, Asset asset, int64_t amount);

	int64_t getNativeAssetReqs();

	void addAssetRequirement(Asset asset, int64_t amount);

	bool checkAvailableBalanceSufficesForNewRequirement(LedgerTxnHeader& header, AbstractLedgerTxn& ltx, Asset asset, int64_t amount);

	AssetMap const& getRequiredAssets() const {
		return mRequiredAssets;
	}

	void cleanZeroedEntries();

	bool isEmpty()
	{
		return mRequiredAssets.empty();
	}
};


} /* namespace stellar */