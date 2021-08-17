#include "speedex/IOCOrderbook.h"

namespace stellar {

IOCOrderbook::IOCOrderbook(AssetPair tradingPair) : mTradingPair(tradingPair) {};


void 
IOCOrderbook::doPriceComputationPreprocessing() {
	PriceCompStats stats;
	for (auto& offer : mOffers) {
		mPrecomputedTatonnementData.push_back(stats); // intentionally starting with 0
		stats.marginalPrice = offer.mMinPrice;
		stats.cumulativeOfferedForSale += offer.mAmount;

		int128_t offerTimesPrice = offer.mAmount * offer.mMinPrice.n;
		offerTimesPrice <<= PriceCompStats::OFFERED_TIMES_PRICE_RADIX;

		stats.cumulativeOfferedForSaleTimesPrice += offerTimesPrice / offer.mMinPrice.d;
	}

	mPrecomputedTatonnementData.push_back(stats);
}

void 
IOCOrderbook::addOffer(IOCOffer offer) {
	mOffers.insert(offer);
}

std::set<IOCOffer>& 
IOCOrderbook::getOffers() {
	return mOffers;
}

void
IOCOrderbook::commitChild(const IOCOrderbook& other) {

	if (tradingPair != other.tradingPair) {
		throw std::runtime_error("merge orderbooks trading pair mismatch!");
	}
	mOffers.insert(other.mOffers.begin(), other.mOffers.end());
	mPrecomputedTatonnementData.clear();
}



} /* stellar */