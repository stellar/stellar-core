#pragma once

#include "ledger/AssetPair.h"

#include "speedex/IOCOrderbook.h"

#include "util/UnorderedMap.h"

namespace stellar {

class IOCOrderbookManager {

	UnorderedMap<AssetPair, IOCOrderbook, AssetPairHash> mOrderbooks;


public:

	void addOffer(AssetPair assetPair, const IOCOffer& offer);

	void commitChild(const IOCOrderbookManager& child);

	void clear();


};

}