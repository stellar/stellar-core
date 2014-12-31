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

std::string& toBase58(uint160 const& b, std::string& retstr);
std::string& toBase58(uint256 const& b, std::string& retstr);
uint256 fromBase58(const std::string& str);
uint160 base58to160(const std::string& str);

uint256 makePublicKey(uint256 const& b);
}

#endif
