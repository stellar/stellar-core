#pragma once

#include "ledger/AssetPair.h"

#include "speedex/IOCOffer.h"

#include <set>
#include <vector>

namespace stellar {

struct IOCOrderbookClearingParams {
	Price clearingRatio;
	int64_t totalSellAmount;
};

class IOCOrderbook {

	struct PriceCompStats {
		Price marginalPrice;
		int64_t cumulativeOfferedForSale;
		int128_t cumulativeOfferedForSaleTimesPrice; // Fractional : Radix 32 bits;

		constexpr static int OFFERED_TIMES_PRICE_RADIX = 32;
	};

	const AssetPair mTradingPair;
	std::set<IOCOffer> mOffers; // sorted by IOCOffer::operator<=>

	std::vector<PriceCompStats> mPrecomputedTatonnementData;

public:
	IOCOrderbook(AssetPair tradingPair);

	void doPriceComputationPreprocessing();

	void addOffer(IOCOffer offer);

	std::set<IOCOffer>& getOffers();

	void commitChild(const IOCOrderbook& child);
};



} /* stellar */