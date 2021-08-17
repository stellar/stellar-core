#include "transactions/CreateSpeedexIOCOfferOpFrame.h"

namespace stellar {

CreateSpeedexIOCOfferOpFrame::CreateSpeedexIOCOfferOpFrame(
	Operation const& op, OperationResult& res, TransactionFrame& parentTx, uint32_t index)
	: OperationFrame(op, res, parentTx)
	, mCreateSpeedexIOCOffer(mOperation.body.createSpeedexIOCOfferOp())
	, mOperationIndex(index)
	{

	}


bool
CreateSpeedexIOCOfferOpFrame::checkMalformed() {
	if (mCreateSpeedexOfferOp.amount <= 0) {
		innerResult.code(CREATE_SPEEDEX_IOC_OFFER_MALFORMED);
		return false;
	}
	auto price = mCreateSpeedexOfferOp.minPrice;
	if (price.n <= 0 || price.d <= 0) {
		innerResult.code(CREATE_SPEEDEX_IOC_OFFER_MALFORMED):
		return false;
	}
	return true;
}

bool
CreateSpeedexIOCOfferOpFrame::checkValidAssetPair(AbstractLedgerTxn& ltx) {
	auto speedexConfig = stellar::loadSpeedexConfigSnapshot(ltx);
	if (!speedexConfig) {
		innerResult().code(CREATE_SPEEDEX_IOC_OFFER_NO_SPEEDEX_CONFIG);
		return false;
	}

	AssetPair tradingPair {
		.buying = mCreateSpeedexIOCOffer.buying,
		.selling = mCreateSpeedexIOCOffer.selling
	};

	if (!speedexConfig.isValidAssetPair(tradingPair)) {
		innerResult().code(CREATE_SPEEDEX_IOC_OFFER_INVALID_TRADING_PAIR);
		return false;
	}
	return true;
}

bool 
CreateSpeedexIOCOfferOpFrame::doApply(AbstractLedgerTxn& ltx)
{
	if (!checkValidAssetPair(ltx)) {
		return false;
	}

	if (!checkMalformed()) {
		return false;
	}

	auto price = mCreateSpeedexIOCOffer.minPrice;
	auto amount = mCreateSpeedexIOCOffer.amount;

	auto hash = IOCOffer::offerHash(
		price,
		getSourceID(),
		getSeqNum(),
		mOperationIndex);
	
	IOCOffer offer(price, amount, hash, getSourceID());

	ltx.addSpeedexIOCOffer(tradingPair, offer);

	auto sourceAccount = stellar::loadAccount(ltx, getSourceID());

	if (!sourceAccount) {
		throw std::runtime_error("commutative preconditions check should have blocked op from nonexistent account");
	}

	auto header = ltx.loadHeader();

	auto sellAsset = mCreateSpeedexIOCOffer.selling;
	if (sellAsset.type() == ASSET_TYPE_NATIVE) {
		auto ok = stellar::addBalance(header, sourceAccount, -amount);
		if (!ok) {
			throw std::runtime_error("commutative preconditions check should have blocked op with insufficent balance");
		}
	} else {
        auto sourceLine = loadTrustLine(ltx, getSourceID(), sellAsset);

        if (!sourceLine)
        {
            throw std::runtime_error("commutative preconditions should block nonexistent trustline");
        }

        if (!sourceLine.addBalance(header, -amount))
        {
            throw std::runtime_error("commutative preconditions should block insufficent trustline balance");
        }
	}
	return true;
}
bool 
CreateSpeedexIOCOfferOpFrame::doCheckValid(uint32_t ledgerVersion)
{
	return checkMalformed();
}

bool 
CreateSpeedexIOCOfferOpFrame::doAddCommutativityRequirements(AbstractLedgerTxn& ltx, AccountCommutativityRequirements& reqs) {

	if (!checkValidAssetPair(ltx)) {
		return false;
	}

	if (!reqs.checkTrustLine(ltx, getSourceID(), mCreateSpeedexIOCOffer.buying)) {
		innerResult().code(CREATE_SPEEDEX_IOC_OFFER_MALFORMED);
		return false;
	}

    if (!reqs.tryAddAssetRequirement(ltx, mCreateSpeedexIOCOffer.selling, mCreateSpeedexIOCOffer.amount))
    {
    	innerResult().code(CREATE_SPEEDEX_IOC_OFFER_INSUFFICIENT_BALANCE);
        return false;
    }
    return true;
}

void
CreateSpeedexIOCOfferOpFrame::insertLedgerKeysToPrefetch(UnorderedSet<LedgerKey>& keys) const {

	auto sourceID = getSourceID();

	auto trustLineKeyGen = [&] (Asset asset) {
		if (asset.type() != ASSET_TYPE_NATIVE) {
			keys.emplace(trustlineKey(sourceID, asset));
		}
	}

	trustLineKeyGen(mCreateSpeedexIOCOffer.selling);
	trustLineKeyGen(mCreateSpeedexIOCOffer.buying);
}


} /* stellar */