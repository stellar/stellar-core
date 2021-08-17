#include "speedex/IOCOrderbookManager.h"

namespace stellar {

void
IOCOrderbookManager::commitChild(const IOCOrderbookManager& child) {
	for (const auto& orderbook : child.mOrderbooks) {
		(mOrderbooks.emplace(orderbook.first, orderbook.first)).first->second.commitChild(orderbook.second);
	}
}

void
IOCOrderbookManager::addOffer(AssetPair assetPair, const IOCOffer& offer) {
	mOrderbooks.emplace(assetPair, assetPair).first->second.addOffer(offer);
}

void
IOCOrderbookManager::clear() {
	mOrderbooks.clear();
}

} /* stellar */
