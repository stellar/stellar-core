// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

%#include "generated/SCPXDR.h"

namespace stellar
{

// messages
typedef opaque uint512[64];
typedef hyper int64;
typedef int int32;
typedef opaque AccountID[32];
typedef opaque Thresholds[4];
typedef string string32<32>;

typedef uint64 SequenceNumber;

enum CurrencyType
{
    CURRENCY_TYPE_NATIVE = 0,
    CURRENCY_TYPE_ALPHANUM = 1
};

union Currency switch (CurrencyType type)
{
case CURRENCY_TYPE_NATIVE:
    void;

case CURRENCY_TYPE_ALPHANUM:
    struct
    {
        opaque currencyCode[4];
        AccountID issuer;
    } alphaNum;

    // add other currency types here in the future
};

// price in fractional representation
struct Price
{
    int32 n; // numerator
    int32 d; // denominator
};
}
