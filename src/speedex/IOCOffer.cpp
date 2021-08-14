#include "speedex/IOCOffer.h"

#include "utils/XDROperators.h"

namespace stellar {

IOCOffer::IOCOffer(int64_t sellAmount, Price minPrice, Hash totalOrderingHash)
	: mSellAmount(sellAmount), mMinPrice(minPrice), mTotalOrderingHash(totalOrderingHash) {}

std::strong_ordering 
IOCOffer::operator<=>(const IOCOffer& other) {
	int64_t lhs = ((int64_t)mMinPrice.n) * ((int64_t) other.mMinPrice.d);
	int64_t rhs = ((int64_t)mMinPrice.d) * ((int64_t) other.mMinPrice.n);

	if (lhs < rhs) {
		return std::strong_ordering::less;
	}
	if (lhs > rhs) {
		return std::strong_ordering::greater;
	}

	if (mTotalOrderingHash < other.mTotalOrderingHash) {
		return std::strong_ordering::less;
	}
	if (mTotalOrderingHash > other.mTotalOrderingHash) {
		return std::strong_ordering::greater;
	}
	return std::strong_ordering::equal;
}


} /* stellar */