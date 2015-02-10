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
    uint32 sequence;
    uint32 ownerCount;
    uint256 *inflationDest;
    opaque thresholds[4]; // weight of master/threshold1/threshold2/threshold3
    Signer signers<>; // do we want some max or just increase the min balance
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

// selling 10A for 15B
struct OfferEntry
{
    uint256 accountID;
    uint32 sequence;

    Currency sell;       // A
    int64 amountSell;    // amount of A for sale

    Currency buy;       // B
    int64 amountBuy;    // amount of B seller wants
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
