// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

%#include "generated/SCPXDR.h"

namespace stellar
{

// messages
typedef opaque uint512[64];
typedef opaque uint256[32];
typedef unsigned hyper uint64;
typedef hyper int64;
typedef unsigned int uint32;
typedef int int32;
typedef opaque AccountID[32];
typedef opaque Signature[64];
typedef opaque Hash[32];
typedef opaque Thresholds[4];

typedef uint64 SequenceNumber;

enum CurrencyType
{
    NATIVE = 0,
    ISO4217 = 1
};

struct ISOCurrencyIssuer
{
    opaque currencyCode[4];
    AccountID issuer;
};

union Currency switch (CurrencyType type)
{
case NATIVE:
    void;

case ISO4217:
    ISOCurrencyIssuer isoCI;

    // add other currency types here in the future
};

// price in fractional representation
struct Price
{
    int32 n; // numerator
    int32 d; // denominator
};
}
