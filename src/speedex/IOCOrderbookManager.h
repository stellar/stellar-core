#pragma once

#include "ledger/AssetPair.h"

#include "speedex/IOCOrderbook.h"

namespace stellar {

class IOCOrderbookManager {

	UnorderedMap<AssetPair, IOCOrderbook> mOrderbooks;


public:

	void addOffer(AssetPair assetPair, const IOCOffer& offer);

	void commitChild(const IOCOrderbookManager& child);

	void clear();


};

}