#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <vector>
#include "generated/StellarXDR.h"
#include "xdrpp/message.h"

namespace stellar
{
typedef std::vector<unsigned char> Blob;

bool isZero(uint256 const& b);

uint256 makePublicKey(uint256 const& b);

// returns true if the Currency value is well formed
bool isCurrencyValid(Currency const& cur);

// returns true if the currencies are the same
bool compareCurrency(Currency const& first, Currency const& second);

void currencyCodeToStr(xdr::opaque_array<4U> const& code, std::string& retStr);

void strToCurrencyCode(xdr::opaque_array<4U>& ret, std::string const& str);

// calculates A*B/C when A*B overflows 64bits
int64_t bigDivide(int64_t A, int64_t B, int64_t C);
// no throw version, returns true if result is valid
bool bigDivide(int64_t& result, int64_t A, int64_t B, int64_t C);

// no throw version, returns true if result is valid
bool bigDivide(uint64_t& result, uint64_t A, uint64_t B, uint64_t C);

bool iequals(std::string const& a, std::string const& b);

bool operator>=(Price const& a, Price const& b);
bool operator>(Price const& a, Price const& b);
bool operator==(Price const& a, Price const& b);
}
