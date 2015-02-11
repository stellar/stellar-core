#pragma once

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

void currencyCodeToStr(const xdr::opaque_array<4U>& code, std::string& retStr);

void strToCurrencyCode(xdr::opaque_array<4U>& ret, const std::string& str);

// calculates A*B/C when A*B overflows 64bits
int64_t bigDivide(int64_t A, int64_t B, int64_t C);

bool iequals(const std::string& a, const std::string& b);

}


