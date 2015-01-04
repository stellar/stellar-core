#ifndef __TYPES__
#define __TYPES__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <vector>
#include "generated/StellarXDR.h"
#include "xdrpp/message.h"

namespace stellar
{
typedef std::vector<unsigned char> Blob;

bool isZero(uint256 const& b);

uint256 makePublicKey(uint256 const& b);

// returns true if the currencies are the same
bool compareCurrency(Currency& first, Currency& second);
}

#endif
