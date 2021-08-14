#pragma once

#include "xdr/Stellar-ledger-entries.h"
#include "ledger/AssetPair.h"

#include <memory>

namespace stellar {

class SpeedexConfigEntryFrame {
	//TODO kill class, this is much better done as util functions

	std::shared_ptr<const LedgerEntry> mSpeedexConfig;

public:
	void setConfig(std::shared_ptr<const LedgerEntry> config);

	operator bool() const {
		return mSpeedexConfig;
	}

	void reset() {
		mSpeedexConfig = nullptr;
	}

	bool isValidAssetPair(AssetPair tradingPair);

};


} /* stellar */