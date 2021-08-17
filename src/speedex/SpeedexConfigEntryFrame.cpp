#include "speedex/SpeedexConfigEntryFrame.h"

namespace stellar {

SpeedexConfigEntryFrame::SpeedexConfigEntryFrame(std::shared_ptr<const LedgerEntry> config)
	: mSpeedexConfig(config) {
		if (config -> data.type() != SPEEDEX_CONFIG) {
			mSpeedexConfig = nullptr;
		}
	}

bool 
SpeedexConfigEntryFrame::isValidAssetPair(AssetPair tradingPair) const
{
	auto const& assets = config -> data.speedexConfig().speedexAssets;
	bool foundBuying = false, foundSelling = false;
	for (auto const& asset : assets) {
		if (tradingPair.selling == asset) {
			foundSelling = true;
		}
		if (tradingPair.buying == asset) {
			foundBuying = true;
		}
	}
	return (foundSelling && foundBuying && (tradingPair.selling != tradingPair.buying));
}


} /* stellar */