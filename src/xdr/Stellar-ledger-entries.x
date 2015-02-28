%#include "generated/Stellar-types.h"

namespace stellar {

enum LedgerEntryType {
    ACCOUNT,
    TRUSTLINE,
    OFFER
};

struct Signer
{
    uint256 pubKey;
    uint32 weight;  // really only need 1byte
};

struct KeyValue
{
    uint32 key;
    opaque value<64>;
};

enum AccountFlags
{ // masks for each flag
    AUTH_REQUIRED_FLAG = 0x1
};

struct AccountEntry
{
    uint256 accountID;
    int64 balance;
    uint32 numSubEntries;
    uint256 *inflationDest;
    opaque thresholds[4]; // weight of master/threshold1/threshold2/threshold3
    Signer signers<>; // do we want some max or just increase the min balance
    uint32 seqSlots<>;
    KeyValue data<>;

    uint32 flags; // require dt, require auth,
};

struct TrustLineEntry
{
    uint256 accountID;
    Currency currency;
    int64 limit;
    int64 balance;
    bool authorized;  // if the issuer has authorized this guy to hold its credit
};

// selling 10A @ 2B/A
struct OfferEntry
{
    uint256 accountID;
    uint64 offerID;
    Currency takerGets;  // A
    Currency takerPays;  // B
    int64 amount;    // amount of A

    Price price;
                    // price of A in terms of B
                    // price*10,000,000
                    // price=AmountB/AmountA=priceNumerator/priceDenominator
                    // price is after fees
    int32 flags;
};

union LedgerEntry switch (LedgerEntryType type)
{
    case ACCOUNT:
        AccountEntry account;

    case TRUSTLINE:
        TrustLineEntry trustLine;

    case OFFER:
        OfferEntry offer;
};

}
