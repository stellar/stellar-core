#pragma once

#include "xdr/Stellar-ledger-entries.h"
#include "ledger/AssetPair.h"

#include <memory>

namespace stellar {

class SpeedexConfigEntryFrame {
	std::shared_ptr<const LedgerEntry> mSpeedexConfig;

public:

	SpeedexConfigEntryFrame(std::shared_ptr<const LedgerEntry> config);

	operator bool() const {
		return (bool) mSpeedexConfig;
	}

	bool isValidAssetPair(const AssetPair& tradingPair) const;

};


} /* stellar */