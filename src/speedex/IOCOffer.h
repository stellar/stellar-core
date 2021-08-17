#pragma once

#include "xdr/Stellar-types.h"

#include <compare>

#include <cstdint>

namespace stellar {

struct IOCOffer {
	int64 mSellAmount;
	Price mMinPrice;
	Hash mTotalOrderingHash;
	AccountID mSourceAccount;

	IOCOffer(int64_t sellAmount, Price minPrice, Hash totalOrderingHash, AccountID sourceAccount);

	std::strong_ordering operator<=>(const IOCOffer& other);

	// should not be changed by feeBumpTx;
	static Hash offerHash(Price price, AccountID sourceAccount, uint64_t sourceSeqNum, uint32_t opIdNum);

};

} /* stellar */