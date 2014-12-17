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

bool isZero(stellarxdr::uint256 const& b);

void hashXDR(xdr::msg_ptr msg, stellarxdr::uint256& retHash);
void hashStr(std::string str, stellarxdr::uint256& retHash);

std::string& toBase58(stellarxdr::uint256 const& b, std::string& retstr);
stellarxdr::uint256 fromBase58(const std::string& str);

stellarxdr::uint256 makePublicKey(stellarxdr::uint256 const& b);
}

#endif
