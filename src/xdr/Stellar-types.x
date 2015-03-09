%#include "generated/SCPXDR.h"

namespace stellar {

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

enum CurrencyType
{
    NATIVE,
    ISO4217
};

struct ISOCurrencyIssuer
{
    opaque currencyCode[4];
    AccountID issuer;
};

union Currency switch(CurrencyType type)
{
    case NATIVE: 
        void;

    case ISO4217: 
        ISOCurrencyIssuer isoCI;

    // add other currency types here in the future
};

struct Price
{
    int32 n;
    int32 d;
};

}
