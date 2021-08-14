#pragma once

#include "xdr/Stellar-types.h"

#include <compare>

#include <cstdint>

namespace stellar {

struct IOCOffer {
	int64 mSellAmount;
	Price mMinPrice;
	Hash mTotalOrderingHash;

	IOCOffer(int64_t sellAmount, Price minPrice, Hash totalOrderingHash);

	std::strong_ordering operator<=>(const IOCOffer& other);

};

} /* stellar */